# Pilot Audit — 0x00405B40 ClampVehicleAttitudeLimits

**Date:** 2026-05-14
**Pool slot:** TD5_pool4
**Port-side function:** `td5_physics_clamp_attitude` @ `td5_physics.c:5607`
**Worktree:** `.claude/worktrees/precise-00405B40` on branch `precise-00405B40`
**Tag:** `pool4_00405B40`
**Caller:** `UpdateVehicleActor (0x00406650)` — called once per tick when `actor->vehicle_mode == 0`.
**Callees:** `MultiplyRotationMatrices3x3 (0x0042DA10)`, `BuildRotationMatrixFromAngles (0x0042E1E0)`.
**Body:** `0x00405B40..0x00405D65` (0x225 bytes / 129 instructions / 75 decompiled lines).

## Function structure (from listing)

Reads two 12-bit display angles and dispatches on the global at `0x00463188`:

```
prologue (0x00405B40-B85):
    EDX = *(uint32)0x00463188            ; "g_cameraMode" in Ghidra; really g_3dCollisionsToggle
    EBX = actor (param)
    EAX = (uint16) actor->display_angle_roll   ; +0x208
    ECX = (uint16) actor->display_angle_pitch  ; +0x20C
    iVar1 = ((EAX - 0x800) & 0xFFF) - 0x800    ; signed[-0x800..+0x7FF] roll
    iVar2 = ((ECX - 0x800) & 0xFFF) - 0x800    ; signed[-0x800..+0x7FF] pitch

dispatch (0x00405B86-88):
    if (EDX == 0) jmp MODE_0_LATCH    ; matrix-recovery branch
    else fall through to MODE_1_NUDGE

MODE_1_NUDGE (0x00405B8E-C3D): soft-nudge then hard-clamp
    ESI = 0x200; EDX = -0x200
    if (roll  < -0x27F)              roll_omega  += ESI    ; +0x200 toward 0
    if (roll  > +0x27F)              roll_omega  += EDX    ; -0x200 toward 0
    if (pitch < -0x2BB)              pitch_omega += ESI
    if (pitch > +0x2BB)              pitch_omega += EDX
    if (roll  < -0x355) { roll_omega  = 0; disp_roll  = -0x355; eul_roll  = -0x35500 }
    if (roll  > +0x355) { roll_omega  = 0; disp_roll  = +0x355; eul_roll  = +0x35500 }
    if (pitch < -0x3A4) { pitch_omega = 0; disp_pitch = -0x3A4; eul_pitch = -0x3A400 }
    if (pitch > +0x3A4) { pitch_omega = 0; disp_pitch = +0x3A4; eul_pitch = +0x3A400 }
    return

MODE_0_LATCH (0x00405C5C-D65): early-out if within limits, else recovery latch
    if (-0x355 <= roll <= +0x355  &&  -0x3A4 <= pitch <= +0x3A4) return
    /* attitude exceeded one of the four bounds -- build delta rotation */
    delta_roll  = (eul_roll  - roll_omega)  >> 8   ; SAR (signed)
    delta_yaw   = (eul_yaw   - yaw_omega)   >> 8
    delta_pitch = (eul_pitch - pitch_omega) >> 8
    BuildRotationMatrixFromAngles(&local_60, {delta_roll, delta_yaw, delta_pitch})
    MultiplyRotationMatrices3x3(&actor->rotation_matrix, local_60, local_60)
    memcpy(actor + 0x180, local_60, 12*4)           ; collision_spin_matrix <- product
    memcpy(actor + 0x150, actor + 0x120, 12*4)      ; saved_orientation    <- rotation_matrix
    actor->vehicle_mode = 1                         ; +0x379
    actor->frame_counter = 0                        ; +0x338  (uint16 write)
    return
```

### Key arithmetic primitive

The angle-recovery idiom `((ushort)val - 0x800 & 0xfff) - 0x800` maps a raw 12-bit
unsigned angle to its signed counterpart in `[-0x800, +0x7FF]`. Critically:

| `val` (uint16) | `((val - 0x800) & 0xFFF) - 0x800` |
|---|---|
| 0x000           | -0x800 |
| 0x7FF           | +0x7FF |
| **0x800**       | **-0x800** (signed wrap) |
| 0x801           | -0x7FF |
| 0xFFF           |  -1    |

The boundary at 0x800 wraps to -0x800. This is a center-shifted unwrap, not an
"if (val > 0x800) val -= 0x1000" reduction — the two differ at exactly
`val == 0x800`.

### `g_cameraMode` is the 3D-collisions toggle

Cross-refs to `0x00463188`:
- Written at `0x004155BD` inside `ScreenMainMenuAnd1PRaceFlow` (frontend writes the
  user's `collisions_3d` save-data dword).
- Written at `0x0041DC8E` inside `ScreenOptionsHub` (frontend toggle handler).
- Read at `0x00405B40` (this function).
- Read at `0x00408295` inside `ApplyVehicleCollisionImpulse`.

Save-data convention: 0 = OFF, 1 = ON. Therefore:
- `*0x00463188 == 0` → user has 3D collisions DISABLED → MODE-0 (matrix recovery)
- `*0x00463188 != 0` → user has 3D collisions ENABLED  → MODE-1 (soft nudge + clamp)

Ghidra's name `g_cameraMode` is a misnomer; it tracks `TD5_GameOptions.collisions_3d`.

## Port mapping

| Original | Port |
|---|---|
| `*0x00463188` (g_cameraMode) | `g_collisions_enabled` (static int32 in td5_physics.c) |
| `actor->display_angle_roll`  (+0x208) | `actor->display_angles.roll` |
| `actor->display_angle_pitch` (+0x20C) | `actor->display_angles.pitch` |
| `actor->angular_velocity_roll`  (+0x1C0) | `actor->angular_velocity_roll` |
| `actor->angular_velocity_yaw`   (+0x1C4) | `actor->angular_velocity_yaw` |
| `actor->angular_velocity_pitch` (+0x1C8) | `actor->angular_velocity_pitch` |
| `actor->euler_accum_roll`  (+0x1F0) | `actor->euler_accum.roll` |
| `actor->euler_accum_yaw`   (+0x1F4) | `actor->euler_accum.yaw` |
| `actor->euler_accum_pitch` (+0x1F8) | `actor->euler_accum.pitch` |
| `actor->rotation_m00..m22` (+0x120) | `actor->rotation_matrix` (TD5_Mat3x3) |
| `actor->saved_orientation_m00` (+0x150) Ghidra | `actor->saved_orientation` (port name matches) |
| `actor->recovery_target_m00` (+0x180) Ghidra | `actor->collision_spin_matrix` (port name; same offset) |
| `actor->vehicle_mode` (+0x379) | `actor->vehicle_mode` |
| `actor->frame_counter` (+0x338) | `actor->frame_counter` |

Note: Ghidra's `saved_orientation_m00`/`recovery_target_m00` labels are
swapped relative to the port's `saved_orientation`/`collision_spin_matrix`.
The port's names match the offsets (`+0x150` and `+0x180` respectively) and
the listing's MOVSD.REP order. Original copies:
- `[EBX+0x180] <- local_60` (the new product matrix) → port `collision_spin_matrix`
- `[EBX+0x150] <- [EBX+0x120]` (the current rotation) → port `saved_orientation`

## Confirmed divergences (port vs original)

### D1 — Boundary value at `display_angle == 0x800` ◆ LOW IMPACT

Port (line 5612-5614):
```c
int32_t roll  = (actor->euler_accum.roll >> 8) & 0xFFF;
int32_t pitch = (actor->euler_accum.pitch >> 8) & 0xFFF;
if (roll  > 0x800) roll  -= 0x1000;
if (pitch > 0x800) pitch -= 0x1000;
```

For `roll == 0x800` exactly: port keeps `+0x800`; original yields `-0x800`. The
condition should be `>= 0x800` (or use the original's center-shift idiom).

Impact: cosmetic — `0x800` corresponds to exactly 180° in 12-bit angle space,
which is the "flipped upside down" attitude. Both `+0x800` and `-0x800`
exceed every roll/pitch threshold, so the branch routing is the same; the
ONLY downstream user is `BuildRotationMatrixFromAngles` in MODE-0, which
treats the input as signed short (so `+0x800` and `-0x800` produce the same
matrix — `cos(180°)` and `sin(180°)` are sign-symmetric).

Verdict: byte-faithful fix is required for static-port mandate even though
runtime impact is nil.

### D2 — Input source differs: `display_angles.*` vs `euler_accum.* >> 8` ◆ NONE

Original reads `(uint16)actor->display_angle_roll` directly. Port reads
`actor->euler_accum.roll >> 8 & 0xFFF`. These are equivalent because
`display_angle_*` is written each tick from `(uint16)(euler_accum >> 8)`
(see `td5_physics_integrate_pose` line ~5070 and `pose_from_physics`).

Verdict: equivalent at any moment when `display_angles` is in sync with
`euler_accum`. The original reads the cached display angle; the port
recomputes it from the accumulator. To be byte-faithful with the listing
input convention, switch to `display_angles` reads.

### D3 — MODE-0 disabled by port ◆ HIGH IMPACT (when triggered)

Port's MODE-0 branch is replaced with a comment claiming the suspension
equilibrium drift would trigger spurious recovery teleports. The
recovery-latch path (build delta matrix, write `collision_spin_matrix`,
copy `rotation_matrix` to `saved_orientation`, set `vehicle_mode=1`,
zero `frame_counter`) is entirely missing.

Static-port mandate (memory `feedback-precise-port-over-approximation`):
the function must port faithfully. Suspension drift is upstream and is
the correct concern of `UpdateVehicleSuspensionResponse` / the suspension
equilibrium fix, not this function.

Verdict: re-implement MODE-0 byte-faithfully. The suspension-drift guard
should be moved upstream (or accepted as a known regression until the
suspension equilibrium is fixed). The pilot doesn't fix the upstream
drift — it only ports this function.

### D4 — Early-out routing inverted vs original ◆ LOGIC EQUIVALENT

Port (MODE-1 nudge branch, line 5637-5639):
```c
if (roll <= roll_nudge && roll >= -roll_nudge &&
    pitch <= pitch_nudge && pitch >= -pitch_nudge)
    return;
```

Original (MODE-1 branch) has NO such early-out — every soft-nudge `if`
condition is independent, and the soft-nudge is applied unconditionally
when its threshold is crossed. Algebraically equivalent for outputs (all
4 conditions false means no add fires), but adds a branch the original
doesn't have. The conditional skip means port is byte-faithful only when
none of the omega writes fire.

Verdict: remove the early-out in MODE-1 to match original's straight-line
8 independent `if`s.

### D5 — `g_collisions_enabled` semantic-inversion bug ◆ UPSTREAM

`td5_physics_set_collisions(int enabled)` (line 6981) writes
`g_collisions_enabled = enabled ? 0 : 1`. With user-collisions ON, port
flag = 0. Original would have `*0x00463188 = 1` for the same user state.

Effect: port's `if (g_collisions_enabled == 0)` matches "user collisions ON"
but the listing's `if (*0x00463188 == 0)` matches "user collisions OFF".

Original semantics (collisions OFF → MODE-0 recovery latch; collisions ON
→ MODE-1 soft nudge) is the physically sensible mapping. Port has the
branches flipped at the SOURCE — flag inversion in the setter, not in
the reader.

Verdict: NOT FIXED IN THIS PILOT — separate upstream binding bug.
The pilot ports the FUNCTION faithfully; downstream of a wrong global
value, the function still produces the result the original would
produce GIVEN THAT INPUT.

### D6 — `frame_counter` write width ◆ NONE

Original writes `MOV word ptr [EBX + 0x338], 0` (uint16). Port struct field
is `int16_t frame_counter` per `td5_actor_struct.h:480`. C assignment of 0
to int16_t is byte-equivalent.

### D7 — `MOVSD.REP` 12 dwords vs `TD5_Mat3x3` size ◆ NONE

Original copies 12 dwords (48 bytes) for each matrix. `TD5_Mat3x3` is 9
floats (36 bytes). The original copies the matrix PLUS 3 trailing
dwords. Looking at the offsets:
- `+0x120` (rotation_matrix) starts a 0x30-byte (48-byte) range up to `+0x14F`
- `+0x150` (saved_orientation) starts a 0x30-byte range up to `+0x17F`
- `+0x180` (collision_spin_matrix) starts a 0x30-byte range up to `+0x1AF`

So the 12-dword copy includes the matrix + 3 trailing dwords at
`+0x144..0x14F` (which is the "translation" portion in
`LoadRenderTranslation` convention). Port's `TD5_Mat3x3` only covers
the 9-float rotation; the trailing 3 floats are SEPARATE fields in the
port (or padding). Need to check.

Verdict: VERIFY in implementation. If port's `TD5_Mat3x3` doesn't cover
all 48 bytes, the trailing 12 bytes are not preserved.

## Audit summary

| ID | Severity | Fix in this pilot? |
|---|---|---|
| D1 — angle 0x800 boundary | LOW (cosmetic) | YES |
| D2 — display_angles vs euler>>8 | NONE (equivalent) | YES (faithful input source) |
| D3 — MODE-0 disabled | HIGH (when triggered) | YES (full re-impl) |
| D4 — MODE-1 early-out | LOGIC-EQUIV | YES (remove early-out) |
| D5 — flag-inversion bug | UPSTREAM | NO (separate scope) |
| D6 — frame_counter width | NONE | (already correct) |
| D7 — 12-dword copy size | UNKNOWN | INVESTIGATE |

## Capture schema for pilot

Per call (one row per tick when actor->vehicle_mode == 0, slot 0 only):

**Keys:** `sim_tick`, `slot`, `caller_ra` (for sanity)

**Inputs:**
- `g_collisions_flag` (the runtime value of `*0x00463188` / port's `g_collisions_enabled`)
- `display_roll_raw`, `display_pitch_raw` (uint16, at entry)
- `iVar1_signed_roll`, `iVar2_signed_pitch` (post-unwrap)
- `eul_roll`, `eul_yaw`, `eul_pitch` (entry values)
- `omega_roll`, `omega_yaw`, `omega_pitch` (entry values)
- `vehicle_mode_pre`, `frame_counter_pre`

**Outputs:**
- `omega_roll_post`, `omega_pitch_post` (soft-nudge effects)
- `display_roll_post`, `display_pitch_post` (hard-clamp effects)
- `eul_roll_post`, `eul_pitch_post`
- `vehicle_mode_post`, `frame_counter_post`
- `branch_taken` (0=skip, 1=MODE-1, 2=MODE-0-latch)
- For MODE-0: `delta_roll`, `delta_yaw`, `delta_pitch` (the SAR-shifted angles
  fed to BuildRotationMatrixFromAngles)
- For MODE-0: 12 floats of `collision_spin_matrix` and 12 floats of
  `saved_orientation` post-write.

Schema is ~50 columns; one row per tick × at most ~30 ticks captured = 30 rows.

## Reference

- Listing: 0x00405B40..0x00405D65 (Ghidra TD5_pool4, 2026-05-14)
- Decompilation: same session, 75 lines
- Port pre-pilot: `td5mod/src/td5re/td5_physics.c:5607-5663`
- Helpers: `td5_render.c` MultiplyRotationMatrices3x3 (line 4694),
  BuildRotationMatrixFromAngles (line 4724)
- Actor struct: `re/include/td5_actor_struct.h` (rotation_matrix +0x120,
  saved_orientation +0x150, collision_spin_matrix +0x180)
