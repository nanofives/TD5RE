# Pilot Audit — 0x00405D70 ResetVehicleActorState

**Date:** 2026-05-14
**Pool slot:** TD5_pool5
**Port-side function:** `td5_physics_reset_actor_state` @ `td5_physics.c:5671`
**Worktree:** `.claude/worktrees/precise-00405D70` on branch `precise-00405D70`
**Callers (6):** `CheckAndUpdateActorCollisionAlignment (0x00409520)`, `IntegrateScriptedVehicleMotion (0x00409D20)`, `InitializeActorTrackPose (0x00434350)`, `UpdateSpecialTrafficEncounter (0x00434DA0)`, `RecycleTrafficActorFromQueue (0x004353B0)`, `InitializeTrafficActorsFromQueue (0x00435940)`.
**Callees (1):** `IntegrateVehiclePoseAndContacts (0x00405E80)` (called once mid-function).
**Body:** 0x00405D70..0x00405E7D (0x10E bytes / 54 instructions). Includes one nested call.

## Function structure (from listing)

```
prologue:                                      ; 0x405D70-D74
    PUSH EBX; XOR EBX,EBX; PUSH ESI
    MOV ESI, [ESP+0xC]                          ; ESI = actor

clear flags:                                    ; 0x405D78-DAF
    MOV byte  [ESI+0x376], 0                    ; surface_contact_flags
    MOV byte  [ESI+0x379], 0                    ; vehicle_mode
    MOV dword [ESI+0x1C0..0x1D4], 0            ; ang_vel_roll, yaw, pitch + lin_vel_x,y,z (6 dwords)
    MOV word  [ESI+0x338], 0                    ; frame_counter (int16)
    MOV byte  [ESI+0x37C], 0                    ; wheel_contact_bitmask  (NOT damage_lockout)

set spawn sentinels:                            ; 0x405DB5-CC
    MOV dword [ESI+0x200], 0xC0000000          ; world_pos.y = -0x40000000 (sentinel)
    MOV byte  [ESI+0x36B], 0x02                ; current_gear = 2
    MOV dword [ESI+0x310], 0x190               ; engine_speed_accum = 400

zero suspension state:                          ; 0x405DD0-E4 (loop, 4 iter)
    LEA EAX, [ESI+0x2DC]                       ; wheel_suspension_pos base
    MOV ECX, 4
.loop:
    MOV [EAX+0x10], EBX                         ; wheel_spring_dv[i] = 0
    MOV [EAX], EBX                              ; wheel_suspension_pos[i] = 0
    ADD EAX, 4
    DEC ECX
    JNZ .loop

compute render pos + zero euler/display:        ; 0x405DE6-E41
    FILD [ESI+0x1FC]                           ; ST(0) = (float)world_pos.x
    MOV EAX, [ESI+0x1F4]                        ; EAX = euler_accum.yaw
    SAR EAX, 8                                  ; arithmetic >> 8 (signed)
    PUSH ESI                                    ; cdecl arg for upcoming CALL
    FMUL [0x45D5E8]                            ; * 1/256.0f  (DAT_0045D5E8 = 0x3B800000)
    MOV [ESI+0x208], BX=0                       ; display_angles.roll = 0
    MOV [ESI+0x1F0], EBX=0                      ; euler_accum.roll = 0
    FSTP [ESI+0x144]                            ; render_pos.x

    MOV [ESI+0x20A], AX                         ; display_angles.yaw = low16(eulerYaw>>8)
    FILD [ESI+0x200]                           ; ST(0) = (float)world_pos.y == -1073741824.0
    MOV [ESI+0x20C], BX=0                       ; display_angles.pitch = 0
    MOV [ESI+0x1F8], EBX=0                      ; euler_accum.pitch = 0
    FMUL [0x45D5E8]
    FSTP [ESI+0x148]                            ; render_pos.y = -4194304.0f
    FILD [ESI+0x204]                           ; (float)world_pos.z
    FMUL [0x45D5E8]
    FSTP [ESI+0x14C]                            ; render_pos.z

call integrate:                                 ; 0x405E47-4B
    CALL IntegrateVehiclePoseAndContacts        ; cdecl(ESI)
                                                ; this ground-snaps via per-wheel refresh

post-integrate (re-zero dynamics):              ; 0x405E4C-7C
    XOR ECX, ECX
    MOV [ESI+0x2FC..0x308], ECX                ; wheel_load_accum[0..3] = 0 (4 dwords)
    ADD ESP, 4                                  ; pop arg
    MOV [ESI+0x1C0], EBX=0                      ; angular_velocity_roll = 0 (RE-zero)
    MOV [ESI+0x1C8], EBX=0                      ; angular_velocity_pitch = 0 (RE-zero)
    MOV [ESI+0x1D0], EBX=0                      ; linear_velocity_y = 0 (RE-zero)
    POP ESI; POP EBX; RET
```

## Exhaustive offset map (every write in the listing)

| asm | offset | field | value |
|---|---|---|---|
| MOV byte | 0x376 | `surface_contact_flags` | 0 |
| MOV byte | 0x379 | `vehicle_mode` | 0 |
| MOV dword | 0x1C0 | `angular_velocity_roll` | 0 |
| MOV dword | 0x1C4 | `angular_velocity_yaw` | 0 |
| MOV dword | 0x1C8 | `angular_velocity_pitch` | 0 |
| MOV dword | 0x1CC | `linear_velocity_x` | 0 |
| MOV dword | 0x1D0 | `linear_velocity_y` | 0 |
| MOV dword | 0x1D4 | `linear_velocity_z` | 0 |
| MOV word | 0x338 | `frame_counter` | 0 |
| MOV byte | 0x37C | `wheel_contact_bitmask` | 0 |
| MOV dword | 0x200 | `world_pos.y` | 0xC0000000 (-0x40000000) |
| MOV byte | 0x36B | `current_gear` | 2 |
| MOV dword | 0x310 | `engine_speed_accum` | 400 |
| loop | 0x2DC..0x2E8 | `wheel_suspension_pos[0..3]` | 0 |
| loop | 0x2EC..0x2F8 | `wheel_spring_dv[0..3]` | 0 |
| FILD+FMUL+FSTP | 0x144 | `render_pos.x` | (float)world_pos.x * 1/256 |
| MOV word | 0x208 | `display_angles.roll` | 0 |
| MOV dword | 0x1F0 | `euler_accum.roll` | 0 |
| MOV word | 0x20A | `display_angles.yaw` | (int16)(euler_accum.yaw >> 8) (no mask!) |
| FILD+FMUL+FSTP | 0x148 | `render_pos.y` | float(world_pos.y==-0x40000000) * 1/256 = -4194304.0f |
| MOV word | 0x20C | `display_angles.pitch` | 0 |
| MOV dword | 0x1F8 | `euler_accum.pitch` | 0 |
| FILD+FMUL+FSTP | 0x14C | `render_pos.z` | (float)world_pos.z * 1/256 |
| CALL | — | `IntegrateVehiclePoseAndContacts` | with ESI |
| zero | 0x2FC..0x308 | `wheel_load_accum[0..3]` | 0 (after integrate) |
| MOV dword | 0x1C0 | `angular_velocity_roll` | 0 (re-zero) |
| MOV dword | 0x1C8 | `angular_velocity_pitch` | 0 (re-zero) |
| MOV dword | 0x1D0 | `linear_velocity_y` | 0 (re-zero) |

**TOTAL writes from listing:** 25 unique offsets (3 re-zeroed → 28 stores total) + 1 callee.

## Confirmed divergences (port vs original)

### D1 — extraneous writes the original never makes  **[REMOVE]**
The port writes the following fields that DO NOT appear in the listing:

| port write | offset | listing? |
|---|---|---|
| `actor->damage_lockout = 0;` | 0x37D | NO — original leaves prior tick's wheel_contact_bitmask in this slot |
| `actor->front_axle_slip_excess = 0;` | ~0x320 | NO |
| `actor->rear_axle_slip_excess = 0;` | ~0x324 | NO |
| `actor->accumulated_tire_slip_x = 0;` | — | NO |
| `actor->accumulated_tire_slip_z = 0;` | — | NO |
| `actor->longitudinal_speed = 0;` | 0x314 | NO |
| `actor->lateral_speed = 0;` | 0x318 | NO |
| `actor->current_slip_metric = 0;` | — | NO |
| `actor->tire_track_emitter_FL/FR/RL/RR = 0xFF;` | 0x371..0x374 | NO |
| `actor->steering_command = 0;` | 0x30C? | NO |
| `actor->encounter_steering_cmd = 0;` | ~0x368 | NO |
| `actor->brake_flag = 0;` | — | NO |
| `actor->handbrake_flag = 0;` | — | NO |
| `actor->center_suspension_pos = 0;` | 0x2CC | NO |
| `actor->center_suspension_vel = 0;` | 0x2D0 | NO |
| `actor->display_angles.roll = 0; pitch = 0;` (in this exact form, OK if value match) | 0x208, 0x20C | listing writes BX=0 — same effect, no divergence |

The "extras" cause state divergence on any caller that depends on residual values surviving the reset. For example, **`InitializeTrafficActorsFromQueue` and `RecycleTrafficActorFromQueue` recycle traffic actors expecting their slip/steering history to persist**; the port nukes them.

### D2 — `display_angles.yaw` mask  **[REMOVE]**
Listing: `SAR EAX,8; MOV [ESI+0x20A], AX` — raw `int16(yaw>>8)`, no 12-bit mask.
Port: `(int16_t)((actor->euler_accum.yaw >> 8) & 0xFFF)`.

For canonical inputs (yaw is 12-bit so `euler_accum.yaw` fits in 0..0xFFFFF), the two outputs agree. For pathological/large yaw the port truncates while the original sign-extends. Should be removed regardless to match the listing byte-for-byte.

### D3 — `(int32_t)0xC0000000` cast  **[NEUTRAL]**
Port: `actor->world_pos.y = (int32_t)0xC0000000;`. Listing: `MOV dword, 0xC0000000`. Equivalent under two's complement; keep but document.

### D4 — `TD5_LOG_I` tail call  **[NEUTRAL]**
Port has a trailing `TD5_LOG_I(LOG_TAG, "reset_actor_state: …");`. Listing has no such call. Logging is conditional + side-effect-free as far as actor state is concerned, but it should be guarded by the project's normal log gates (it already is, via `TD5_LOG_I`).

### D5 — `wheel_load_accum` post-zero is **CORRECT** in port
Both port and listing zero `wheel_load_accum[0..3]` AFTER the integrate call. Port already does this correctly inline. Keep.

## Fixes to apply (this pilot)

1. **Remove every write listed in D1** — the function must touch only the offsets the listing touches.
2. **Remove the `& 0xFFF` mask** from the yaw seeding (D2).
3. Keep the (int32_t) cast on `0xC0000000` — gcc warns on literal `0xC0000000` to `int32_t` without it.
4. Keep the trailing log line (gated by `TD5_LOG_I`).
5. Re-order the stores to match the listing's interleave only where it has observable effect — the FILD/FMUL/FSTP pipeline executes alongside integer MOVs, but since all are independent writes to disjoint actor fields, source order does not affect output.

## Risk

LOW. ResetVehicleActorState runs once per spawn/respawn, so:
- Per-frame cost: 0
- Failure mode if a "removed" extra-write was actually needed: stale tire_track_emitter IDs / slip / steering state on respawn. Mitigated because:
  - tire_track_emitter is reset to 0xFF by `td5_vfx_release_actor_emitters` separately on race-init.
  - slip / steering / lateral_speed are overwritten unconditionally each tick by the dynamics path before they're read.
  - encounter_steering_cmd is rewritten by the AI script VM each tick.
  - center_suspension_pos/vel — overwritten by integrate_suspension each tick.

If post-fix capture surfaces a regression, restore the specific extra write with a Ghidra-cite for the dependent caller.

## Capture schema for pilot

Per call (1 row per slot per call — function fires only at spawn/respawn):

**Keys:** `sim_tick`, `slot`, `caller_addr` (for the 6 caller dispatch)
**Inputs:** `world_pos_x/y/z` (PRE — to verify the sentinel writes), `euler_accum_yaw` (PRE), `current_gear` (PRE), `engine_speed` (PRE)
**Outputs (post-call):**
- `wcb` (wheel_contact_bitmask)
- `dam_lockout` (damage_lockout — should NOT be zeroed by reset)
- `world_pos_y` (must be -0x40000000 right after the sentinel store; will be ground-snapped later by integrate)
- `world_pos_y_final` (post-CALL value — actually integrate writes this)
- `render_pos_x/y/z`
- `display_angles_roll/yaw/pitch`
- `euler_accum_roll/yaw/pitch`
- `frame_counter`
- `surface_contact_flags`, `vehicle_mode`
- `current_gear`, `engine_speed_accum`
- `wheel_suspension_pos[0..3]`, `wheel_spring_dv[0..3]`, `wheel_load_accum[0..3]`
- `ang_vel_roll/yaw/pitch`, `lin_vel_x/y/z`

Most rows will be identical to spawn-baseline since the function only fires at a respawn-event. The diff window is `tick=0` (spawn) and any respawn ticks.

## Next actions

1. Edit port to remove D1 extras and D2 mask.
2. Add `tools/frida_pool5_00405D70.js` probe at entry+exit.
3. Add port-side trace emitter to `td5_pilot_trace_00405D70.{c,h}`.
4. Build worktree.
5. Run Edinburgh baseline + capture; diff.

## Reference

- Listing: 0x00405D70..0x00405E7D (Ghidra TD5_pool5, 2026-05-14, 54 instructions)
- Decompilation: same session, 47 lines
- Port (pre-fix): `td5mod/src/td5re/td5_physics.c:5671-5783`
- Field map: `re/include/td5_actor_struct.h` (offsets cross-checked via `_Static_assert`)
- Callee: `IntegrateVehiclePoseAndContacts (0x00405E80)` — out of scope for this pilot (will be diffed by 00405E80 pilot session).
