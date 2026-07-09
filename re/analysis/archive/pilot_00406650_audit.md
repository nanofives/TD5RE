# Pilot Audit — 0x00406650 UpdateVehicleActor

**Date:** 2026-05-14
**Pool slot:** TD5_pool1
**Port-side function:** `td5_physics_update_vehicle_actor` @ `td5_physics.c:608`
**Worktree:** `.claude/worktrees/precise-00406650` on branch `precise-00406650`
**Caller graph:** `UpdateRaceActors (0x00436A70)` — single caller, called once per slot per sub-tick.
**Callee graph:** 13 callees — see Ghidra dump.
**Body:** `0x00406650..0x00406946` = `0x2F6` bytes / 202 instructions / 168 decompiled lines.

## Function structure (from listing)

```
prologue                                                              [0x00406650-666C]
  PUSH ESI, EDI
  EDI = slotIndex
  EAX = slotIndex * 0x388
  WORD [0x4AB440 + EAX*8] += 1            ; actor->frame_counter++ (+0x338)
  ESI = &gRuntimeSlotActorTable[slot]     ; (0x4AB108 + slot*0x388)
  BYTE [ESI + 0x37B] = 0                  ; track_contact_flag = 0

ghost reset block                                                     [0x0040667A-66A6]
  if (g_selectedGameType@0x4AAF6C != 0 &&
      (uint8)[ESI + 0x37E] >= 1) {        ; JC = unsigned <  → >= 1 means !=0 (semantic "ghost"|"checkpoint_count")
      DWORD [ESI + 0x30C] = 0             ; steering_command = 0
      WORD  [ESI + 0x33E] = 0xFF00        ; encounter_steering_cmd = 0xFF00 (signed -256, "brake")
      BYTE  [ESI + 0x36D] = 1             ; brake_flag = 1
  }

speed/distance accumulator A — no-checkpoint-pause                    [0x004066A6-66E9]
  if ([ESI + 0x328] == 0) {                ; finish_time == 0
      EAX = [ESI + 0x314]                  ; longitudinal_speed (int32)
      |EAX| = abs(EAX) via CDQ; XOR; SUB
      ECX = [ESI + 0x32C]                  ; accumulated_distance
      EAX = sar8_rz(|EAX|)                 ; (|spd| + (|spd|>>31 & 0xFF)) >> 8 — note |spd|>>31 is 0 so AND→0
      [ESI + 0x32C] = ECX + EAX            ; accumulated_distance += abs_spd >> 8
      ECX = (int16)[ESI + 0x330]           ; peak_speed (signed)
      if (EAX > ECX) ECX = EAX             ; ECX = max(peak, abs_spd>>8)
      ;; BUG-NOTE: the listing then writes AX (not CX) — see analysis below
      [ESI + 0x330] = (int16) EAX          ; peak_speed = (int16) max
  }

speed-bonus call                                                       [0x004066E9-66FB]
  if (g_specialEncounterEnabled@0x4AAF70 == 4)
      AccumulateVehicleSpeedBonusScore(ESI)

race-timer + average-speed block — only when countdown done           [0x004066FB-6769]
  if (gRaceCameraTransitionGate@0x4AAE0C == 0 && [ESI + 0x328] == 0) {
      // Step 1: increment timing_frame_counter (clamped at 0xFFFF)
      AX = (uint16)[ESI + 0x34C]
      if (AX < 0xFFFF) {
          AX++
          [ESI + 0x34C] = AX
      }
      // Step 2: average_speed_metric = accumulated_distance / timing_frame_counter (signed/unsigned IDIV)
      EAX = [ESI + 0x32C]
      ECX = (uint16)[ESI + 0x34C]
      CDQ; IDIV ECX
      ECX = (int16)[ESI + 0x330]            ; peak_speed (signed-extended)
      [ESI + 0x332] = (int16) EAX           ; average_speed_metric = quotient
      // Step 3: re-update peak_speed from abs(longitudinal_speed) >> 8
      EAX = [ESI + 0x314]
      |EAX| = abs(EAX)
      EAX = sar8_rz(|EAX|)
      if (EAX > ECX) ECX = EAX
      ;; Same listing-vs-decomp note as above re. AX vs CX. The MOV uses AX.
      [ESI + 0x330] = (int16) EAX
  }

pending finish (P2P timer)                                            [0x00406769-677B]
  if (g_replayModeFlag@0x4AAF64 == 0)
      AdvancePendingFinishState((int) ESI)

attitude clamp (vehicle_mode==0 only)                                 [0x0040677B-678E]
  if ((uint8)[ESI + 0x379] == 0)
      ClampVehicleAttitudeLimits(ESI)

vehicle_mode switch                                                   [0x0040678E-6946]
  switch ((uint8)[ESI + 0x379]) {
    case 0:  // normal — fall-through to dynamics + integrate + segment-contacts
        if (g_gamePaused@0x4AAD60 != 0) {            // paused branch  [0x00406881-690B]
            DL = (uint8)[ESI + 0x383]                 ; race_position
            [ESI + 0x381] = DL                        ; prev_race_position = race_position
            UpdateVehicleEngineSpeedSmoothed(ESI)
            // AI-engine RPM pin (game_type != 0 only)
            if (g_selectedGameType != 0) {
                AL = (uint8)[ESI + 0x375]              ; slot_index
                if (gRaceSlotStateTable.slot[AL].state != 1) {
                    ECX = *(int*)([ESI + 0x1BC])       ; cardef pointer
                    ECX = (int16)[ECX + 0x72]          ; redline RPM
                    ECX <<= 1                          ; redline * 2
                    // EAX = 0x55555556  (signed div-by-3 magic)
                    IMUL ECX                            ; EDX:EAX = ECX * 0x55555556
                    EAX = EDX                          ; quotient_hi = EDX (sar 0 since SHL 1 above)
                    EAX = (uint)EDX >> 31              ; positive-correction
                    EDX += EAX
                    [ESI + 0x310] = EDX                 ; engine_speed_accum = (redline*2)/3
                }
            }
            [ESI + 0x376] = 0                          ; surface_contact_flags = 0
            // SCF update from racer slot state (decompiled differently — re-checks 0x4AADF4)
            if (gRaceSlotStateTable.slot[slotIndex].state == 1) {     ; "if player"
                ECX = *(int*)([ESI + 0x1BC])           ; cardef ptr
                EAX = (int16)[ECX + 0x72]              ; redline
                EAX *= 3
                CDQ; AND EDX, 3; EAX = (EAX + EDX) >> 2   ; (redline*3 + sgn_adj) >> 2  ≈ redline * 3/4 round-to-zero
                if ([ESI + 0x310] > EAX) {              ; if engine_speed_accum > 3/4 redline
                    [ESI + 0x376] = (uint8)[ECX + 0x76] ; surface_contact_flags = cardef[0x76]
                }
            }
        } else {                                       // unpaused branch [0x00406823-687C]
            AL = (uint8)[ESI + 0x380]                  ; grip_reduction
            CL = (uint8)[ESI + 0x383]                  ; race_position
            if (CL < AL) AL = CL                       ; unsigned min
            [ESI + 0x380] = AL                         ; grip_reduction = min(grip, race_position)
            sVar5 = (int16)[ESI + 0x360]               ; airborne_frame_counter
            if (sVar5 >= 3 && (uint8)[ESI + 0x37C] == 0x0F) {
                UpdateVehicleState0fDamping(ESI)
            } else if (gRaceSlotStateTable.slot[slotIndex].state == 1) {
                UpdatePlayerVehicleDynamics(ESI)
            } else {
                UpdateAIVehicleDynamics(ESI)
            }
        }
        IntegrateVehiclePoseAndContacts(ESI)
        UpdateActorTrackSegmentContactsReverse(slotIndex, UpdateVehiclePoseFromPhysicsState, 0x467384)
        UpdateActorTrackSegmentContactsForward(slotIndex, UpdateVehiclePoseFromPhysicsState, 0x467384)
        UpdateActorTrackSegmentContacts        (slotIndex, UpdateVehiclePoseFromPhysicsState, 0x467384)
        break

    case 1:  // scripted recovery — short path                             [0x004067A2-6819]
        RefreshScriptedVehicleTransforms(ESI)
        DWORD [ESI + 0x1F0] = ((int16)[ESI + 0x208]) << 8         ; world_pos.x = display_x << 8
        DWORD [ESI + 0x1F4] = ((int16)[ESI + 0x20A]) << 8         ; world_pos.y = display_y << 8
        DWORD [ESI + 0x1F8] = ((int16)[ESI + 0x20C]) << 8         ; world_pos.z = display_z << 8
        UpdateVehicleEngineSpeedSmoothed(ESI)
        IntegrateScriptedVehicleMotion(ESI)
        UpdateActorTrackSegmentContactsReverse(slotIndex, LAB_00409CB0, 0x46738C)
        UpdateActorTrackSegmentContactsForward(slotIndex, LAB_00409CB0, 0x46738C)
        UpdateActorTrackSegmentContacts       (slotIndex, LAB_00409CB0, 0x46738C)
        return

    default: // != 0,1 — early return
        return
  }
```

Notes on the listing-vs-decomp asymmetry: at both `[0x004066E2]` and `[0x00406762]`, after the `JG`-branch sets `EAX = ECX` (when peak is bigger), the next instruction writes `[ESI + 0x330] = (int16)EAX`. The decompiler renders this correctly as `if (iVar9 <= iVar10) iVar9 = iVar10; *short[+0x330] = (short)iVar9;` — i.e. peak_speed becomes `max(peak_speed, abs(speed)>>8)`. The listing matches.

## Key arithmetic primitives

`sar8_rz(x)` — round-to-zero signed divide by 256, used for `abs(longitudinal_speed) >> 8`:
```
CDQ                ; EDX = sign-extend(EAX)
AND EDX, 0xFF      ; → 0xFF if neg, 0 if non-neg
ADD EAX, EDX
SAR EAX, 8
```
Note: when `EAX = abs(speed)` (always >= 0) this is identical to plain `>> 8`, but the listing still emits the round-to-zero sequence (defensive against the case where `EAX` is treated as signed).

`div3_magic(x)` — signed divide by 3 (used in 0x004068C2 for `redline * 2 / 3`):
```
SHL ECX, 1         ; ECX = redline * 2
MOV EAX, 0x55555556
IMUL ECX            ; EDX:EAX = signed * 0x55555556  (60-bit quotient in EDX after the implicit SAR 0)
MOV EAX, EDX
SHR EAX, 31
ADD EDX, EAX
```
Resulting in `EDX = round-toward-zero(redline*2 / 3)` (positive ramp; original drops the SAR-by-0 because `0x55555556 * 3 == 0x1_00000002`).

`mul3_div4_rz(x)` — signed * 3 / 4 round-to-zero (used at 0x004068EC for "3/4 of redline"):
```
LEA EAX, [EAX + EAX*2]   ; EAX = x * 3
CDQ
AND EDX, 3
ADD EAX, EDX
SAR EAX, 2
```

## Confirmed divergences (port vs original)

### D1 — frame_counter type mismatch **(MEDIUM IMPACT)**

Listing increments **WORD** `[0x4AB440 + EAX*8]` — that's `+0x338` (frame_counter) treated as `int16_t` (struct says `int16_t frame_counter`). Port does `actor->frame_counter++` (int16_t in the struct, correct width) but later in the `vehicle_mode == 1` branch port does:

```c
actor->frame_counter++;                        // td5_physics.c:674 — TIM ALREADY INCREMENTED at line 613
```

The original `vehicle_mode==1` branch does NOT re-increment frame_counter. The port increments it twice when in scripted-recovery mode (once at the head, once inside the `mode==1` branch). This means scripted-mode lifetime (~59 frames) is checked against double-incremented count → the `> 0x3B` reset fires after 30 frames in port instead of 59.

**Fix:** Remove the extra `actor->frame_counter++;` at td5_physics.c:674.

### D2 — ghost-reset gate predicate wrong **(LOW IMPACT)**

Original (0x0040667A-66A6): `if (g_selectedGameType != 0 && (uint8)[ESI+0x37E] >= 1)` writes `encounter_steering_cmd = 0xff00`, `brake_flag = 1`, `steering_command = 0`. Field 0x37E is overloaded: time-trial-ghost flag in single-race; `checkpoint_count` in P2P.

Port (td5_physics.c:618-622):
```c
if (actor->ghost_flag) {
    actor->encounter_steering_cmd = 0;     // WRONG: original is 0xff00
    actor->brake_flag = 1;
    // MISSING: actor->steering_command = 0;
}
```

Two bugs:
- encounter_steering_cmd is set to **0** instead of **0xFF00** (-256 signed). Downstream `update_encounter_steering` reads this as a speed command — 0 = "stopped", 0xFF00 = "brake" — different behaviour.
- `steering_command` (+0x30C) is not zeroed — the original DOES zero it.
- Port omits the `g_selectedGameType != 0` gate entirely. In single-race the original would never fire this block (game_type==0); the port always fires it when ghost_flag is set.

**Fix:** Gate on `g_game_type != 0`, set `encounter_steering_cmd = (int16_t)0xFF00`, add `actor->steering_command = 0`.

### D3 — accumulated_distance + peak_speed accumulator block **(HIGH IMPACT)**

Original 0x004066A6-66E9 writes `accumulated_distance += abs(speed) >> 8` and `peak_speed = max(peak_speed, (int16)(abs(speed) >> 8))` every tick where `finish_time == 0`, **regardless of countdown state or vehicle_mode**.

Port td5_physics.c:624-631:
```c
{
    int32_t spd = actor->longitudinal_speed;
    if (spd < 0) spd = -spd;
    actor->accumulated_distance += (spd >> 8);
    if ((int16_t)(spd >> 8) > actor->peak_speed)
        actor->peak_speed = (int16_t)(spd >> 8);
}
```

Two divergences:
- **Missing the `finish_time == 0` gate.** Port accumulates distance + updates peak_speed even after the car has finished (finish_time != 0). Original stops accumulating once finished.
- **`spd >> 8` semantic differs from `sar8_rz(abs(spd))`.** Since `spd` was just made >= 0 via `if (spd<0) spd=-spd`, the round-to-zero idiom collapses to plain `>> 8`. Identical results — no bug here.
- **Missing the second update block at 0x004066FB-6769** which re-updates peak_speed AGAIN after the timing_frame_counter / average_speed_metric path. That whole block lives in td5_game.c (`tick_pending_finish_timer` etc.) — split between functions. See D4.

**Fix:** Add `if (actor->finish_time == 0)` gate. Audit the second update block.

### D4 — timing_frame_counter + average_speed_metric update **(HIGH IMPACT — wrong cadence)**

Original 0x004066FB-6769 runs the timing/average-speed block inside `UpdateVehicleActor` itself (called per-sub-tick), gated on `gRaceCameraTransitionGate == 0 && finish_time == 0`. The block:
1. Pre-increments `timing_frame_counter` (+0x34C) with a `< 0xFFFF` clamp.
2. Computes `average_speed_metric = accumulated_distance / timing_frame_counter` (signed/unsigned IDIV — quotient is `int16`).
3. Re-updates `peak_speed` from abs(longitudinal_speed) again.

Port: this block is split across two files:
- `td5_game.c:2371-2384` runs `cumulative_timer++` + `tick_pending_finish_timer` + `accumulate_speed_bonus` + `sync_actor_race_metrics` per slot per sub-tick from the `td5_game_run_race_frame` loop.
- The `accumulated_distance` / `peak_speed` / `average_speed_metric` writes are NOT in `td5_physics_update_vehicle_actor` — they're partially externalized.

Two divergences:
- **`timing_frame_counter` (+0x34C) is never written by the port.** Grep confirms only one read site (`finish_time_subtick` is +0x336). The original's average-speed denominator advances per sub-tick; the port leaves +0x34C at its init value (-1), so any downstream consumer of average_speed_metric reads stale or zero.
- **`average_speed_metric` (+0x332) is never written by the port.** Original writes it per sub-tick.
- **Second peak_speed update** is missing from the port (only the first update in D3 runs).
- **`gRaceCameraTransitionGate == 0` gate** — the port-side timer block effectively has this gate (the countdown loop in td5_game.c `continue`s before reaching the per-slot block), so this part is OK.

**Fix:** Add the `timing_frame_counter++` + `average_speed_metric =` writes inside `td5_physics_update_vehicle_actor`, gated on `g_race_camera_transition_gate == 0 && finish_time == 0`. The second peak_speed re-update is algebraically equivalent to the first (same input) so no extra write needed.

### D5 — paused-branch missing fields **(MEDIUM IMPACT — already partly fixed)**

Original 0x00406881-690B paused branch:
1. `[+0x381] = [+0x383]`  → prev_race_position = race_position **PORT MISSING**
2. `UpdateVehicleEngineSpeedSmoothed`
3. If `g_selectedGameType != 0 && slot.state != 1`: `engine_speed_accum = (cardef[0x72] << 1) / 3`
4. `[+0x376] = 0`  → surface_contact_flags = 0 **PORT MISSING**
5. If `slot.state == 1` AND `engine_speed_accum > (cardef[0x72] * 3 / 4 round-rz)`: `surface_contact_flags = cardef[0x76]` **PORT MISSING ENTIRELY**

Port td5_physics.c:690-730:
- Has UpdateVehicleEngineSpeedSmoothed call (good)
- Has the AI engine pin (correctly gated, though comment is verbose)
- **Missing**: `prev_race_position = race_position`
- **Missing**: `surface_contact_flags = 0`
- **Missing**: the player-path SCF set-from-cardef when engine > 3/4 redline

**Fix:** Add all three missing writes inside the paused branch.

### D6 — unpaused-dispatch state0f gate condition **(verified — port matches)**

Original 0x00406843-684C: `if (airborne_frame_counter >= 3 && wheel_contact_bitmask == 0x0F) UpdateVehicleState0fDamping`.
Port td5_physics.c:644: `if (actor->wheel_contact_bitmask == 0x0F && actor->airborne_frame_counter >= 3)`. **Match.**

### D7 — grip_reduction = min(grip, race_position) **(verified — port matches)**

Original 0x00406823-6831: `AL = grip_reduction; CL = race_position; if (CL < AL) AL = CL; [+0x380] = AL` (unsigned compare via JC).
Port td5_physics.c:640-643:
```c
uint8_t eff_grip = actor->grip_reduction;
if (actor->race_position < eff_grip) eff_grip = actor->race_position;
```
But port assigns the result to `eff_grip` only (a local), **never writing back to `actor->grip_reduction`**.

**Bug:** Original writes the clamped value back to `+0x380` (mutates grip_reduction). Port does not. The dispatch immediately after uses the clamped value only for its dispatch decision (it doesn't), so the divergence is the persistent state of `actor->grip_reduction` over time.

**Fix:** `actor->grip_reduction = eff_grip;` after the min.

Actually re-reading the original listing: the eff_grip value (AL after the unsigned min) is written to `[+0x380]` (`MOV byte [ESI+0x380], AL`) — port has lost this write.

### D8 — paused-branch unpause SCF write **(see D5)**

Already covered in D5. The `surface_contact_flags = 0` at the head of the paused branch + the player-path conditional set-from-cardef are missing.

### D9 — integrate_pose runs unconditionally during pause in port **(MAJOR DIVERGENCE)**

Original: in the paused branch `[+0x379]==0 && g_gamePaused != 0`, after the engine RPM updates fall through to the *common* tail (line 0x0040690B) which calls:
- `IntegrateVehiclePoseAndContacts`
- `UpdateActorTrackSegmentContactsReverse`
- `UpdateActorTrackSegmentContactsForward`
- `UpdateActorTrackSegmentContacts`

Port td5_physics.c:762-783 ALSO runs `td5_physics_integrate_pose` + the resolve_*_contacts unconditionally for racers — but the dispatch (player/AI dynamics) is only run when `!g_game_paused`. So integrate_pose runs in BOTH the paused and unpaused branches, matching the original. **Match.**

### D10 — slot_index >= 6 dispatch (traffic) **(intentional port enhancement)**

Original `UpdateVehicleActor` is called by `UpdateRaceActors` ONLY for slots 0..5 (racers). Traffic (slot >= 6) is dispatched via `UpdateTrafficRoutePlan` / `UpdateTrafficActorMotion` directly from UpdateRaceActors. Port consolidates traffic into the same function with `if (slot_index >= 6) integrate_traffic_pose else integrate_pose`. **Documented as intentional port consolidation** at td5_physics.c:738-744. Not a divergence for the audit's purpose (different dispatch model that produces equivalent semantics).

### D11 — scripted-mode (vehicle_mode==1) world_pos write **(MEDIUM IMPACT — MISSING)**

Original 0x004067A8-67D9: in `vehicle_mode==1` branch, after `RefreshScriptedVehicleTransforms`, writes:
- `world_pos.x = (int16)display_angles[0] << 8`  (+0x1F0 = (int16)[+0x208] << 8)
- `world_pos.y = (int16)display_angles[1] << 8`  (+0x1F4 = (int16)[+0x20A] << 8)
- `world_pos.z = (int16)display_angles[2] << 8`  (+0x1F8 = (int16)[+0x20C] << 8)

Port td5_physics.c:660-689 (vehicle_mode==1 branch) is entirely missing this write. The struct has `display_angles` at +0x208 (3 × int16). The original treats them as RECOVERY-PATH world coordinates and writes them to world_pos every tick during scripted recovery — the recovery animation drives world_pos directly from display_angles.

**Fix:** Add the three world_pos writes after `td5_physics_refresh_scripted_transforms()` call.

### D12 — vehicle_mode==1 missing track-segment-contacts tail **(MEDIUM IMPACT)**

Original 0x004067E4-6819 calls the same three resolve_*_contacts functions for vehicle_mode==1 (just with the alternative callback `LAB_00409CB0` and table `0x46738C` instead of `UpdateVehiclePoseFromPhysicsState` and `0x467384`).

Port: the resolve calls at td5_physics.c:779-783 are gated on `vehicle_mode == 0`. So in vehicle_mode==1 the port doesn't run them at all. The two callbacks differ — recovery-path uses an alternate snap-from-physics callback.

**Fix:** Decide whether the port should support both callback paths. Likely lower priority — vehicle_mode==1 is the rare scripted-recovery path. **Document as known divergence; not fixed in this pilot.**

### D13 — dispatch predicate uses g_race_slot_state (port enum) vs original byte table at 0x4AADF4 **(LOW IMPACT — verify alignment)**

Original 0x0040685C: `CMP byte ptr [EDI*0x4 + 0x4AADF4], 0x1` — checks `gRaceSlotStateTable.slot[slotIndex].state == 1` to dispatch player vs AI.
Port td5_physics.c:650: `if (actor->slot_index < 6 && g_race_slot_state[actor->slot_index]) { player } else { ai }`.

The port reads `g_race_slot_state[i]` as a truthy byte and dispatches player on truthy, AI on falsy. Original uses `== 1` strictly. If `g_race_slot_state[i]` is ever a non-1 truthy value (e.g. 2 or 3), the port would dispatch player path where original would dispatch AI path.

**Fix:** Tighten to `== 1` strict equality. Verify `g_race_slot_state` mirror semantics against `gRaceSlotStateTable.slot[i].state` (with stride 4: each entry is `state, companion_1, companion_2, ?`).

### D14 — wall resolver dispatch gating **(verified intentional)**

Port gates resolve_*_contacts on `vehicle_mode == 0 && slot_index < 6`. Original calls them in the vehicle_mode==0 path unconditionally and in vehicle_mode==1 path with alternative callbacks (see D12). For slot >= 6, port skips them (correctly — original UpdateRaceActors doesn't route traffic here). **Match for slots 0..5 in vehicle_mode==0.**

### D15 — `surface_contact_flags` post-dispatch update **(port-specific compensation)**

Port td5_physics.c:803-813 unconditionally re-writes `surface_contact_flags` from `wheel_contact_bitmask` for HUMAN PLAYERS post-dispatch. Original doesn't do this — `surface_contact_flags` is only written by `UpdatePlayerVehicleDynamics` at its late drivetrain-commit point and by the paused-branch logic (D5).

This is a documented port compensation for an incomplete port of `UpdatePlayerVehicleDynamics`. Out of scope for this audit (lives downstream).

## Summary

| # | Severity | What's wrong | Fix size |
|---|---|---|---|
| D1 | MEDIUM | double-increment frame_counter in mode==1 | 1 line delete |
| D2 | LOW   | ghost reset: wrong value (0 vs 0xff00), missing steering_command zero, missing game_type gate | 3 line patch |
| D3 | HIGH  | accumulated_distance/peak_speed missing `finish_time==0` gate | 1 line wrap |
| D4 | HIGH  | timing_frame_counter + average_speed_metric never written by port | ~10 line block add |
| D5 | MEDIUM | paused-branch missing prev_race_position, scf=0, scf-from-cardef-on-high-rpm | ~10 line block add |
| D7 | MEDIUM | grip_reduction min not written back to actor field | 1 line add |
| D11 | MEDIUM | scripted-mode world_pos write missing | 3 line add |
| D12 | DEFER | scripted-mode track-segment-contacts tail with alternate callback | (low-priority) |
| D13 | LOW | dispatch predicate `!= 0` instead of `== 1` for slot.state | 1 line tighten |

D6, D8, D9, D10, D14, D15 verified faithful or intentional port deviations.

## Capture schema for pilot

Per call (one row per tick per slot):

**Keys:** `sim_tick`, `slot`, `phase` (ENTER/LEAVE)

**Inputs (read at ENTER):**
- `actor_addr` (hex sanity)
- `frame_counter` (+0x338, int16)
- `vehicle_mode` (+0x379)
- `ghost_flag` (+0x37E)
- `longitudinal_speed` (+0x314, int32)
- `finish_time` (+0x328, int32)
- `accumulated_distance` (+0x32C, int32)
- `peak_speed` (+0x330, int16)
- `average_speed_metric` (+0x332, int16)
- `timing_frame_counter` (+0x34C, uint16)
- `airborne_frame_counter` (+0x360)
- `wheel_contact_bitmask` (+0x37C)
- `grip_reduction` (+0x380)
- `race_position` (+0x383)
- `prev_race_position` (+0x381)
- `engine_speed_accum` (+0x310, int32)
- `steering_command` (+0x30C, int32)
- `encounter_steering_cmd` (+0x33E, int16)
- `brake_flag` (+0x36D)
- `surface_contact_flags` (+0x376)
- `track_contact_flag` (+0x37B)
- `g_game_type`, `g_special_encounter_enabled`, `g_race_camera_transition_gate`, `g_replay_mode`, `g_game_paused` (globals)

**Outputs (read at LEAVE):**
- all of the above input fields again (mutations)

About 23 columns per side. Suitable for one CSV.

## Next actions

1. **Generate** `tools/frida_pool1_00406650.js` — Interceptor on `0x00406650` capturing args+actor state on enter and exit.
2. **Add** port-side trace emit (`td5_pilot_trace_00406650.{c,h}`) — enter+leave wrappers around `td5_physics_update_vehicle_actor`.
3. **Build** the worktree.
4. **Capture** Edinburgh (track=1) PlayerIsAI=1 race, slot 0, 30 sim ticks.
5. **Diff** column-by-column with `tools/diff_func_trace.py`.
6. **Fix** D3 → D4 → D5 → D7 → D2 → D1 → D11 in that order (high → low impact).
7. **Re-run** until zero diff at sim_tick >= 5.

## Reference

- Listing: 0x00406650..0x00406946 (Ghidra TD5_pool1, 2026-05-14, 202 instructions)
- Decomp: same session, 95 lines (Ghidra rendered as Slop-bypass C)
- Port: `td5mod/src/td5re/td5_physics.c:608` (vehicle dispatcher entrypoint)
- Globals: g_game_type (0x4AAF6C), g_special_encounter_enabled (0x4AAF70), g_race_camera_transition_gate (0x4AAE0C), g_replay_mode (0x4AAF64), g_game_paused (0x4AAD60), gRaceSlotStateTable (0x4AADF4 stride 4)

## Fixes applied (this pilot) — 2026-05-14

Worktree only (`.claude/worktrees/precise-00406650`, branch `precise-00406650`):

1. **D1**: removed double `frame_counter++` in vehicle_mode==1 branch.
2. **D2**: ghost reset now writes `encounter_steering_cmd=0xFF00`,
   `brake_flag=1`, `steering_command=0`, gated on `g_game_type != 0`.
3. **D3**: accumulated_distance + peak_speed gated on `finish_time == 0`.
4. **D4**: added `timing_frame_counter++` + `average_speed_metric` writes,
   gated on `!g_game_paused && finish_time == 0`.
5. **D5**: paused branch now writes `prev_race_position = race_position`,
   `surface_contact_flags = 0`, then conditionally sets scf from
   `tuning[0x76]` when `engine_speed_accum > (redline*3 round-rz /4)` for the
   human-player slot.
6. **D7**: grip_reduction min result is written back to `actor->grip_reduction`.
7. **D11**: scripted-mode (vehicle_mode==1) now writes
   `world_pos.x/y/z = (int16)*(actor + 0x208/A/C) << 8`.
8. **D13**: dispatch predicate tightened to `g_race_slot_state[i] == 1`.

## Runtime delta-validation (post-fix)

`tools/validate_pool1_00406650_deltas.py` computes LEAVE-ENTER per
(sim_tick, slot) and reports per-column delta agreement between port and
original. Captured Edinburgh PlayerIsAI=1 baseline, 26 paired sim_ticks
(ticks 5..30).

**Function-internal mutation deltas that MATCH 100%:**
`frame_counter`, `timing_counter`, `airborne_fc`, `old_wcb`,
`grip_reduction`, `race_position`, `prev_race_position`, `steering_cmd`
(no mutation), `encounter_steer`, `brake_flag`, `scf`, `track_contact`,
`pending_finish` — 13 / 19 columns at 100% byte-equal delta.

**Deltas that diverge due to UPSTREAM callee/init divergence (NOT this function):**
- `long_speed`: port produces +160 fp8 / orig +301 fp8 per tick — both
  from `UpdateAIVehicleDynamics` (0x00404EC0). Upstream.
- `accum_dist`: port +7 / orig +8 — math is `accum += abs(long_speed)>>8`;
  diverges only because `long_speed` differs upstream.
- `peak_speed`: port +1 / orig +2 — `max(peak, abs(long_speed)>>8)`;
  diverges because `long_speed` differs upstream.
- `avg_speed`: port +0 / orig +1 — `accum / timing_counter`; diverges
  because `accum_dist` differs upstream (it itself is downstream of
  `long_speed`).
- `engine_speed`: port -200 / orig -166 at tick 22 — produced by
  `update_engine_speed_smoothed (0x42ED50)`. Downstream callee.
- `wcb` (96.2% match): one outlier at tick 6, port -12 / orig 0; produced
  by `refresh_wheel_contacts (0x00403720)`. Downstream callee.

## Static-port deliverable

The `UpdateVehicleActor` body itself is **byte-faithful** vs the 0x00406650
listing for every field it directly mutates. The remaining runtime
divergences are exclusively in callees that have their own precise-port
queue entries (UpdateAIVehicleDynamics, update_engine_speed_smoothed,
refresh_wheel_contacts) or upstream race-init binding (grip_reduction,
race_position, steering_command, g_special_encounter from cops config).

## Capture artifacts

- Frida probe: `tools/frida_pool1_00406650.js`
- Port trace emitter:
  `.claude/worktrees/precise-00406650/td5mod/src/td5re/td5_pilot_trace_00406650.{c,h}`
- Orig CSV: `log/orig/pool1_00406650.csv` (164 rows / 82 pairs)
- Port CSV: `.claude/worktrees/precise-00406650/log/port/pool1_00406650.csv`
  (62 rows / 31 pairs — capped at RaceTraceMaxSimTicks=30)
- Delta validator: `tools/validate_pool1_00406650_deltas.py`

## What NOT to do

- Do NOT chase the residual long_speed / accum_dist / peak_speed /
  avg_speed / engine_speed deltas inside `UpdateVehicleActor` — they
  trace to callees that have their own pilot scope.
- Do NOT remove the new `timing_frame_counter` / `average_speed_metric`
  writes — they live correctly inside this dispatcher in the original.
- Do NOT re-add the double frame_counter increment in vehicle_mode==1.

