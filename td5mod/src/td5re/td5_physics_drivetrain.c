/* ========================================================================
 * td5_physics_drivetrain.c -- Engine, transmission, drive torque, gravity
 *
 * Split out of td5_physics.c (P1-B step 3, 2026-07-02). Byte-faithful ports:
 *   - UpdateVehicleEngineSpeedSmoothed (0x0042ED50) / accumulator (0x42EDF0)
 *   - UpdateAutomaticGearSelection (0x42EF10) (+ no-kick init variant)
 *   - ComputeDriveTorqueFromGearCurve (0x42F030) + gear-1 accel helper
 *   - ApplySteeringTorqueToWheels (0x42EEA0)
 *   - ApplyReverseGearThrottleSign (0x42F010) / ComputeReverseGearTorque (0x403C80)
 *   - ComputeVehicleSurfaceNormalAndGravity (0x42EBF0)
 * Cross-TU seam: td5_physics_internal.h (PRIVATE).
 * ======================================================================== */

#include "td5_physics.h"
#include "td5_ai.h"
#include "td5_track.h"
#include "td5_render.h"   /* td5_render_get_vehicle_mesh */
#include "td5_sound.h"    /* td5_sound_play_at_position (Tier 2 recovery SFX) */
#include "td5_input.h"    /* td5_input_ff_collision (wall/prop impact FF) */
#include "td5_vfx.h"      /* td5_vfx_queue_prop_break (TD6 prop debris) */
#include "td5_race_state.h"  /* [LAYERING 2026-07-06] read-only race queries (was td5_game.h) */
#include "td5_arcade.h"   /* arcade collision mult / ghost / wrecking-ball / launch */
#include "td5_damage.h"   /* [CAR DAMAGE] health from impacts, knockout freeze, handling penalty */
#include "td5_laneassist.h" /* optional lane-assist steering aid (port-only, default OFF) */
#include "td5_platform.h"
#include "td5_trace.h"    /* inner-tick physics_trace stages */
#include "td5_carparam.h" /* shared carparam field map + heaviness math (weight mechanics) */
#include "td5_config.h"   /* shared TD5RE_* env-knob accessors (td5_env_int/float/flag_*) */
/* V2V trace headers are included unconditionally: their obb_corner_test /
 * collision_detect_full call sites below are ungated, so the snapshot types
 * must always be declared. The emitters self-stub to no-ops under
 * TD5RE_RELEASE (release build does not link the v2v trace modules). */
#include "td5re.h"
#include "td5_physics_internal.h"  /* PRIVATE core<->assists seam (SAR_RZ, tuning accessors, assist decls) */

/* Include the full actor struct for field-level access.
 * The build system must add TD5RE/re/include to the include path (-I). */
#include "../../../re/include/td5_actor_struct.h"

#include <string.h>  /* memset, memcpy */
#include <math.h>    /* cos, sin */
#include <stdlib.h>  /* abs */
#include <stdio.h>   /* FILE/fopen/fprintf — used by the [Trace] TrafficEdgePen
                      * CSV mirror at traffic_edge_pen (no-op when flag is 0).
                      * [TRACE 2026-05-24 traffic-edge-pen-cluster] */

#define LOG_TAG "physics"


extern void *g_actor_base;
extern uint8_t *g_actor_table_base;

/* ======== [split] Engine & Transmission + Surface Normal & Gravity (moved verbatim from td5_physics.c) ======== */
/* ========================================================================
 * Engine & Transmission
 * ======================================================================== */

/* [CONFIRMED @ 0x0042ED50] Byte-faithful with orig UpdateVehicleEngineSpeedSmoothed.
 * L5 promotion 2026-05-18 (small-tier sweep). Line-for-line listing port;
 * brake/neg-throttle → idle 400; else target = (redline-400)*throttle>>8 + 400,
 * SAR-4 slew (up clamp 400, dead down clamp 200), upper redline clamp omitted.
 *
 * --- UpdateVehicleEngineSpeedSmoothed (0x0042ED50) ---
 *
 * Byte-faithful port of FUN_0042ED50 (RE: TD5_pool11 read-only listing,
 * audited 2026-05-14). Mirrors the original x86 control flow line-for-line:
 *
 *   0x42ED50  PUSH ESI                          ; entry
 *   0x42ED51  MOV  ESI, [ESP+8]                 ; actor
 *   0x42ED55  MOV  EAX, [ESI+0x1bc]             ; tuning_data_ptr
 *   0x42ED5B  MOVSX ECX, WORD [EAX+0x72]        ; ECX = redline (int)
 *   0x42ED5F  MOV  AL,  BYTE [ESI+0x36d]        ; AL  = brake_flag
 *   0x42ED65  TEST AL, AL
 *   0x42ED67  JNZ  0x42ED9A                     ; brake → idle (400)
 *   0x42ED69  MOV  DX,  WORD [ESI+0x33e]        ; DX  = encounter_steering_cmd
 *   0x42ED70  TEST DX, DX
 *   0x42ED73  JL   0x42ED9A                     ; throttle<0 → idle (400)
 *   0x42ED75  MOVSX EDX, DX                     ; EDX = throttle (int)
 *   0x42ED78  LEA  EAX, [ECX-0x190]             ; EAX = redline - 400
 *   0x42ED7E  IMUL EAX, EDX                     ; EAX *= throttle
 *   0x42ED81  CDQ
 *   0x42ED82  AND  EDX, 0xff                    ; round-toward-zero bias
 *   0x42ED88  ADD  EAX, EDX
 *   0x42ED8A  SAR  EAX, 8                       ; >> 8 (signed, round->0)
 *   0x42ED8D  CMP  ECX, EAX
 *   0x42ED8F  JGE  0x42ED93
 *   0x42ED91  MOV  EAX, ECX                     ; EAX = min(EAX, redline)
 *   0x42ED93  ADD  EAX, 0x190                   ; target = step + 400
 *   0x42ED98  JMP  0x42ED9F
 *   0x42ED9A  MOV  EAX, 0x190                   ; idle target = 400
 *   0x42ED9F  MOV  ECX, [ESI+0x310]             ; ECX = engine_speed_accum
 *   0x42EDA5  CMP  EAX, ECX
 *   0x42EDA7  JLE  0x42EDCA                     ; target <= rpm → down path
 * UP path (target > rpm):
 *   0x42EDA9  SUB  EAX, ECX                     ; EAX = target - rpm (>0)
 *   0x42EDAB  CDQ
 *   0x42EDAC  AND  EDX, 0xf                     ; round bias (no-op for >0)
 *   0x42EDAF  ADD  EAX, EDX
 *   0x42EDB1  SAR  EAX, 4                       ; step = delta >> 4
 *   0x42EDB4  CMP  EAX, 0x190
 *   0x42EDB9  JLE  0x42EDE1                     ; step <= 400 → store path
 *   0x42EDBB  MOV  EAX, 0x190                   ; clamp step to 400
 *   0x42EDC0  ADD  ECX, EAX                     ; rpm += step
 *   0x42EDC2  MOV  [ESI+0x310], ECX             ; store
 *   0x42EDC8  POP ESI; RET
 * DOWN path (target <= rpm):
 *   0x42EDCA  SUB  EAX, ECX                     ; EAX = target - rpm (<=0)
 *   0x42EDCC  CDQ
 *   0x42EDCD  AND  EDX, 0xf                     ; round bias = 0xf when <0
 *   0x42EDD0  ADD  EAX, EDX
 *   0x42EDD2  SAR  EAX, 4                       ; step = delta >> 4 (<=0)
 *   0x42EDD5  CMP  EAX, 0xc8
 *   0x42EDDA  JLE  0x42EDE1                     ; always-taken in practice
 *   0x42EDDC  MOV  EAX, 0xc8                    ; (dead) clamp to 200
 *   0x42EDE1  ADD  ECX, EAX                     ; rpm += step (negative)
 *   0x42EDE3  MOV  [ESI+0x310], ECX             ; store
 *   0x42EDE9  POP ESI; RET
 *
 * Notes preserved from prior port audit:
 *   - No post-store clamp (rpm is not bounded against redline or 400 after
 *     the slew — original 0x42EDC2/0x42EDE3 stores rpm unconditionally).
 *   - Down-path clamp to 200 at 0x42EDDC is unreachable: in the down branch
 *     EAX is the signed (target-rpm)>>4 which is <= 0, so the JLE EAX,0xc8
 *     at 0x42EDDA is always taken. We preserve the clamp anyway so the C
 *     mirrors the listing exactly.
 *
 * Field offsets verified vs td5_types.h:
 *   +0x1bc  tuning_data_ptr        +0x310  engine_speed_accum
 *   +0x33e  encounter_steering_cmd +0x36d  brake_flag
 *   tuning[+0x72] = redline (int16)
 */
void update_engine_speed_smoothed(TD5_Actor *actor)
{
    /* MOVSX ECX, WORD [tuning+0x72]  [@ 0x42ED5B] */
    int32_t redline = (int32_t)PHYS_S(actor, PHYS_REDLINE_RPM);

    int32_t target;
    /* TEST AL,AL / JNZ + TEST DX,DX / JL  [@ 0x42ED65, 0x42ED70] */
    if (actor->brake_flag != 0 || (int16_t)actor->encounter_steering_cmd < 0) {
        /* MOV EAX, 0x190  [@ 0x42ED9A] */
        target = 0x190;
    } else {
        /* MOVSX EDX, DX  [@ 0x42ED75] */
        int32_t throttle = (int32_t)(int16_t)actor->encounter_steering_cmd;
        /* LEA EAX,[ECX-0x190]; IMUL EAX,EDX  [@ 0x42ED78,0x42ED7E] */
        int32_t v = (redline - 0x190) * throttle;
        /* CDQ; AND EDX,0xff; ADD EAX,EDX; SAR EAX,8  [@ 0x42ED81-0x42ED8A]
         * Signed round-toward-zero divide by 256. */
        v = (v + (((int32_t)((uint32_t)v >> 31) ? 0xff : 0))) >> 8;
        /* CMP ECX,EAX / JGE / MOV EAX,ECX  [@ 0x42ED8D-0x42ED91]
         * EAX = min(EAX, redline). */
        if (redline < v) v = redline;
        /* ADD EAX, 0x190  [@ 0x42ED93] */
        target = v + 0x190;
    }

    /* MOV ECX,[ESI+0x310]  [@ 0x42ED9F] */
    int32_t rpm = actor->engine_speed_accum;

    /* CMP EAX,ECX / JLE 0x42EDCA  [@ 0x42EDA5-0x42EDA7] */
    int32_t step;
    if (target > rpm) {
        /* UP path: SUB EAX,ECX  [@ 0x42EDA9] */
        int32_t delta = target - rpm;
        /* CDQ; AND EDX,0xf; ADD EAX,EDX; SAR EAX,4  [@ 0x42EDAB-0x42EDB1]
         * delta is strictly > 0 here, so the round bias is 0; plain >>4. */
        step = (delta + (((int32_t)((uint32_t)delta >> 31) ? 0xf : 0))) >> 4;
        /* CMP EAX,0x190 / JLE / MOV EAX,0x190  [@ 0x42EDB4-0x42EDBB] */
        if (step > 0x190) step = 0x190;
    } else {
        /* DOWN path: SUB EAX,ECX  [@ 0x42EDCA] — delta <= 0 */
        int32_t delta = target - rpm;
        /* Round-toward-zero divide by 16 for negative delta. */
        step = (delta + (((int32_t)((uint32_t)delta >> 31) ? 0xf : 0))) >> 4;
        /* CMP EAX,0xc8 / JLE / MOV EAX,0xc8  [@ 0x42EDD5-0x42EDDC]
         * Unreachable in practice — step <= 0 here — but kept for fidelity. */
        if (step > 0xc8) step = 0xc8;
    }

    /* ADD ECX,EAX; MOV [+0x310],ECX  [@ 0x42EDC0/EDC2 or 0x42EDE1/EDE3] */
    actor->engine_speed_accum = rpm + step;
}

/* [CONFIRMED @ 0x0042EDF0] Byte-faithful with orig UpdateEngineSpeedAccumulator.
 * L5 promotion 2026-05-18 (small-tier sweep). Line-for-line listing port;
 * gear==1 → UpdateVehicleEngineSpeedSmoothed; else target = abs(speed>>8) *
 * gear_ratio[gear] * 45 SAR-12 + 400; delta > 800 fast-down -200, < -800
 * fast-up +200, smooth (target-rpm) SAR-2; upper redline clamp.
 *
 * --- UpdateEngineSpeedAccumulator (0x0042EDF0) ---
 *
 * Byte-faithful port of FUN_0042EDF0 (RE: TD5_pool7 read-only listing,
 * audited 2026-05-14). Mirrors the original x86 control flow line-for-line:
 *
 *   0x42EDF0  PUSH ESI                          ; entry
 *   0x42EDF1  MOV  ESI, [ESP+8]                 ; ESI = actor (param_1)
 *   0x42EDF5  XOR  EAX, EAX
 *   0x42EDF7  MOV  AL,  BYTE [ESI+0x36b]        ; AL  = current_gear
 *   0x42EDFD  PUSH EDI
 *   0x42EDFE  MOV  EDI, EAX                     ; EDI = gear (zero-extended)
 *   0x42EE00  CMP  EDI, 0x1
 *   0x42EE03  JNZ  0x42EE11                     ; gear != 1 → forward path
 *   0x42EE05  PUSH ESI
 *   0x42EE06  CALL 0x0042ed50                   ; UpdateVehicleEngineSpeedSmoothed
 *   0x42EE0B  ADD  ESP, 0x4
 *   0x42EE0E  POP  EDI; POP ESI; RET            ; gear==1: neutral helper + ret
 * Forward path (gear != 1):
 *   0x42EE11  MOV  EAX, [ESI+0x314]             ; EAX = longitudinal_speed (int32)
 *   0x42EE17  SAR  EAX, 0x8                     ; EAX = speed >> 8 (signed arith)
 *   0x42EE1A  CDQ                               ; EDX = sign(EAX)
 *   0x42EE1B  XOR  EAX, EDX
 *   0x42EE1D  SUB  EAX, EDX                     ; EAX = abs(speed >> 8)
 *   0x42EE1F  MOV  EDX, [ESP+0x10]              ; EDX = param_2 (tuning_data_ptr)
 *   0x42EE23  MOVSX EDX, WORD [EDX+EDI*2+0x2e]  ; EDX = gear_ratio (signed int16)
 *   0x42EE28  MOV  ECX, [ESI+0x310]             ; ECX = engine_speed_accum (rpm)
 *   0x42EE2E  IMUL EAX, EDX                     ; EAX *= gear_ratio
 *   0x42EE31  LEA  EAX, [EAX+EAX*4]             ; EAX *= 5
 *   0x42EE34  LEA  EAX, [EAX+EAX*8]             ; EAX *= 9  (cumulative *45 = 0x2D)
 *   0x42EE37  CDQ
 *   0x42EE38  AND  EDX, 0xfff                   ; round-toward-zero bias for SAR 12
 *   0x42EE3E  ADD  EAX, EDX
 *   0x42EE40  SAR  EAX, 0xc                     ; EAX = (abs*ratio*45) >> 12 (signed r->0)
 *   0x42EE43  ADD  EAX, 0x190                   ; target = step + 400
 *   0x42EE48  MOV  EDX, ECX                     ; EDX = rpm
 *   0x42EE4A  SUB  EDX, EAX                     ; EDX = delta = rpm - target
 *   0x42EE4C  CMP  EDX, 0x320
 *   0x42EE52  JLE  0x42EE5C                     ; delta <= 0x320 → not fast-down
 *   0x42EE54  SUB  ECX, 0xc8                    ; fast-down: rpm -= 200
 *   0x42EE5A  JMP  0x42EE79                     ; → clamp/store
 *   0x42EE5C  CMP  EDX, 0xfffffce0              ; (-800)
 *   0x42EE62  JGE  0x42EE6C                     ; delta >= -800 → smooth
 *   0x42EE64  ADD  ECX, 0xc8                    ; fast-up: rpm += 200
 *   0x42EE6A  JMP  0x42EE79                     ; → clamp/store
 * Smooth path (-800 <= delta <= 0x320):
 *   0x42EE6C  SUB  EAX, ECX                     ; EAX = target - rpm (= -delta)
 *   0x42EE6E  CDQ
 *   0x42EE6F  AND  EDX, 0x3                     ; round-toward-zero bias for SAR 2
 *   0x42EE72  ADD  EAX, EDX
 *   0x42EE74  SAR  EAX, 0x2                     ; step = (target-rpm) >> 2 (signed r->0)
 *   0x42EE77  ADD  ECX, EAX                     ; rpm += step
 * Clamp/store:
 *   0x42EE79  MOV  EAX, [ESI+0x1bc]             ; EAX = tuning_data_ptr (actor field)
 *   0x42EE7F  MOVSX EAX, WORD [EAX+0x72]        ; EAX = redline (int16)
 *   0x42EE83  CMP  ECX, EAX
 *   0x42EE85  JLE  0x42EE89
 *   0x42EE87  MOV  ECX, EAX                     ; rpm = min(rpm, redline)
 *   0x42EE89  POP  EDI
 *   0x42EE8A  MOV  [ESI+0x310], ECX             ; store rpm
 *   0x42EE90  POP  ESI; RET
 *
 * Notes:
 *   - Original takes a SECOND param (tuning_data_ptr) loaded from [ESP+0x10].
 *     Both callers (0x00404030 UpdatePlayerVehicleDynamics, 0x00404EC0
 *     UpdateAIVehicleDynamics) cache EBX = [actor+0x1bc] and push it as the
 *     second arg, so param_2 == actor->tuning_data_ptr in practice. The port
 *     uses get_phys(actor) (which reads +0x1bc) and reads the gear ratio at
 *     phys[+0x2e + gear*2] — equivalent in effect.
 *   - abs() is computed on the SHIFTED value (`abs(speed >> 8)`), NOT on the
 *     raw speed. For negative speed this rounds DOWN before abs, which
 *     differs from `abs(speed) >> 8` by 1 in the absolute-value bin for
 *     speeds that are not a multiple of 256. The prior port used
 *     `abs(speed) >> 8`; this version mirrors the listing.
 *   - Both SAR steps (>>12 and >>2 in the smooth path) include the explicit
 *     CDQ+AND+ADD round-toward-zero bias used by the original — required for
 *     bit-exact parity on negative intermediates (the >>12 step's intermediate
 *     EAX is unsigned in practice because abs() preceded it, so the bias is
 *     a no-op there, but kept for structural fidelity).
 *   - Fast-down threshold is `delta > 0x320` (i.e. >= 0x321), from
 *     `CMP EDX,0x320; JLE smooth`. Prior port used `> 0x321` (off-by-one) —
 *     fixed here to match the listing exactly.
 *   - Smooth-path bias `+ (sign? 3 : 0)` before `>> 2` was missing from the
 *     prior port; added so the divide-by-4 rounds toward zero (matters when
 *     target < rpm by an amount not divisible by 4).
 *   - Final clamp is UPPER-only against redline. No 400 floor (original does
 *     not clamp the lower bound here — see 0x42EE83 single CMP/JLE/MOV).
 *
 * Field offsets verified vs td5_types.h:
 *   +0x1bc  tuning_data_ptr            +0x314  longitudinal_speed
 *   +0x310  engine_speed_accum         +0x36b  current_gear (byte)
 *   tuning[+0x2e + gear*2] = gear_ratio (int16)
 *   tuning[+0x72]          = redline    (int16)
 */
void td5_physics_update_engine_speed(TD5_Actor *actor)
{
    int16_t *phys = get_phys(actor);
    if (!phys) return;

    /* MOVZX EDI, BYTE [ESI+0x36b]  [@ 0x42EDF5-0x42EDFE] — gear is treated as
     * an unsigned byte index (XOR EAX,EAX / MOV AL,...). */
    int32_t gear = (int32_t)(uint32_t)(uint8_t)actor->current_gear;

    /* CMP EDI,1 / JNZ  [@ 0x42EE00-0x42EE03] — gear==1 → neutral helper. */
    if (gear == 1) {
        update_engine_speed_smoothed(actor);
        return;
    }

    /* MOV EAX,[ESI+0x314]  [@ 0x42EE11] */
    int32_t lspd = actor->longitudinal_speed;
    /* SAR EAX,8  [@ 0x42EE17] — arithmetic shift, rounds toward -inf for neg. */
    int32_t lspd_shr8 = (int32_t)(lspd >> 8); /* signed >> in two's complement */
    /* CDQ; XOR EAX,EDX; SUB EAX,EDX  [@ 0x42EE1A-0x42EE1D] — abs(EAX). */
    int32_t abs_shr8 = lspd_shr8 < 0 ? -lspd_shr8 : lspd_shr8;

    /* MOVSX EDX,WORD [param_2 + gear*2 + 0x2e]  [@ 0x42EE23] — gear ratio.
     * param_2 is the caller-cached tuning_data_ptr (== actor->tuning_data_ptr). */
    int32_t gear_ratio = (int32_t)PHYS_S(actor, PHYS_GEAR_RATIO_BASE + gear * 2);

    /* MOV ECX,[ESI+0x310]  [@ 0x42EE28] */
    int32_t rpm = actor->engine_speed_accum;

    /* IMUL EAX,EDX; LEA*5; LEA*9  [@ 0x42EE2E-0x42EE34] — EAX = abs*ratio*45. */
    int32_t prod = abs_shr8 * gear_ratio * 0x2d;

    /* CDQ; AND EDX,0xfff; ADD EAX,EDX; SAR EAX,12  [@ 0x42EE37-0x42EE40]
     * — signed round-toward-zero divide by 4096. */
    int32_t step12 = (prod + (((int32_t)((uint32_t)prod >> 31)) ? 0xfff : 0)) >> 12;

    /* ADD EAX,0x190  [@ 0x42EE43] — target = step + 400. */
    int32_t target = step12 + 0x190;

    /* MOV EDX,ECX; SUB EDX,EAX  [@ 0x42EE48-0x42EE4A] — delta = rpm - target. */
    int32_t delta = rpm - target;

    /* CMP EDX,0x320 / JLE  [@ 0x42EE4C-0x42EE52] — fast-down on delta > 0x320. */
    if (delta > 0x320) {
        /* SUB ECX,0xc8  [@ 0x42EE54] */
        rpm = rpm - 0xc8;
        /* JMP clamp/store  [@ 0x42EE5A] */
    } else if (delta < -800) {
        /* CMP EDX,-800 / JGE smooth → fall-through fast-up
         * ADD ECX,0xc8  [@ 0x42EE64] */
        rpm = rpm + 0xc8;
        /* JMP clamp/store  [@ 0x42EE6A] */
    } else {
        /* Smooth path  [@ 0x42EE6C-0x42EE77] */
        /* SUB EAX,ECX  — EAX = target - rpm (= -delta). */
        int32_t toward = target - rpm;
        /* CDQ; AND EDX,3; ADD EAX,EDX; SAR EAX,2 — signed r->0 divide by 4. */
        int32_t s = (toward + (((int32_t)((uint32_t)toward >> 31)) ? 3 : 0)) >> 2;
        /* ADD ECX,EAX. */
        rpm = rpm + s;
    }

    /* Clamp/store  [@ 0x42EE79-0x42EE90] — upper clamp at redline, no floor. */
    int32_t redline = (int32_t)PHYS_S(actor, PHYS_REDLINE_RPM);
    if (rpm > redline) rpm = redline;

    actor->engine_speed_accum = rpm;
}

/* Round-to-zero signed divide by 256 — byte-exact port of the 0x0042EF10
 * idiom: CDQ ; AND EDX, 0xFF ; ADD EAX, EDX ; SAR EAX, 8.
 *
 *   For x >= 0: plain SAR by 8 (low byte truncated).
 *   For x <  0 with (x & 0xFF) != 0: (x + 0xFF) >> 8 — one unit closer to zero
 *     than plain SAR (truncated division by 256). */
static inline int32_t sar8_rz_42EF10(int32_t x) {
    return ((x < 0) ? (x + 0xFF) : x) >> 8;
}

/* --- UpdateAutomaticGearSelection (0x0042EF10) ---
 *
 * Byte-exact port from listing 0x0042EF10..0x0042F008 (73 instructions).
 *
 * Automatic transmission FSM. Promotes reverse→first when throttle goes
 * positive, force-reverses on negative throttle, applies upshift/downshift
 * based on per-gear RPM thresholds, and on upshift fires a drivetrain
 * kick across the four wheel_spring_dv slots.
 *
 *   actor + 0x33E  encounter_steering_cmd   int16 (signed throttle)  → EDX
 *   actor + 0x36B  current_gear             uint8                     → AL/BL
 *   actor + 0x310  engine_speed_accum       int32                     → EDI
 *   actor + 0x314  longitudinal_speed       int32                     → EBX
 *   actor + 0x2EC..0x2F8  wheel_spring_dv[4] int32 (FL/FR/RL/RR)
 *   phys + 0x3E + gear*2  upshift threshold int16  (indexed by CACHED gear)
 *   phys + 0x4E + gear*2  downshift threshold int16 (indexed by CACHED gear)
 *   phys + 0x68           drive_torque_mult  int16
 *   DAT_00467394          g_gearTorqueTable  int32[9] = {0,0,0x100,0xC0,
 *                                            0x80,0x40,0x20,0x10,0}
 *
 * KEY semantics from the listing:
 *
 *   1. CACHED GEAR INDEXING.  At 0x0042EF1D the function `XOR EAX,EAX` then
 *      `MOV AL,[ECX+0x36B]` zero-extends current_gear into EAX. This cached
 *      EAX value is used for BOTH the upshift threshold read at 0x0042EF52
 *      (`MOVSX EBX,[ESI+EAX*2+0x3E]`) and the downshift threshold read at
 *      0x0042EFF1 (`MOVSX EDX,[ESI+EAX*2+0x4E]`), AND for the `CMP EAX,0x8`
 *      gate at 0x0042EF5F and `CMP EAX,0x2` gate at 0x0042EFFA. The cache
 *      is NEVER refreshed across the function body — even after the
 *      reverse→2 promotion writes memory at 0x0042EF47, EAX stays at 0.
 *
 *   2. DRIVETRAIN KICK.  On upshift the original spreads a per-gear torque
 *      pulse across wheel_spring_dv via the table at 0x00467394, indexed
 *      by the NEW post-upshift gear (BL after INC at 0x0042EF78).
 *      +0x2EC (FL) and +0x2F0 (FR) ADD k; +0x2F4 (RL) and +0x2F8 (RR)
 *      SUB k. The formula is:
 *          tmp1 = sar8_rz(phys[0x68] * throttle * 0x1A)
 *          tmp2 = tmp1 * g_gearTorqueTable[new_gear & 0xFF]
 *          k    = sar8_rz(tmp2)
 */
void td5_physics_auto_gear_select(TD5_Actor *actor)
{
    int16_t *phys = get_phys(actor);
    if (!phys) return;

    /* Listing 0x0042EF14: MOVSX EDX, [ECX+0x33E]  (signed throttle). */
    int32_t throttle = (int32_t)actor->encounter_steering_cmd;

    /* Listing 0x0042EF1D-21: XOR EAX,EAX ; MOV AL,[ECX+0x36B].
     * EAX holds the zero-extended current_gear and is NEVER refreshed. */
    uint32_t gear_cached = (uint32_t)actor->current_gear;

    /* Listing 0x0042EF28: MOV EDI, [ECX+0x310]  (engine_speed_accum). */
    int32_t rpm = actor->engine_speed_accum;

    /* Listing 0x0042EF1F-3A: TEST EDX,EDX ; JGE 0x42EF3B.
     * Negative throttle → write gear=0 (reverse) and RET. */
    if (throttle < 0) {
        actor->current_gear = (uint8_t)0;
        return;
    }

    /* Listing 0x0042EF3B-4D: if (EAX == 0) check throttle, promote to 2.
     *   TEST EAX,EAX ; JNZ 0x42EF4E
     *   TEST EDX,EDX ; JLE 0x42F005   (cleanup-RET when throttle <= 0)
     *   MOV BYTE PTR [ECX+0x36B], 0x2  (gear=2 in memory only) */
    if (gear_cached == 0u) {
        if (throttle <= 0) return;
        actor->current_gear = (uint8_t)2;
        /* gear_cached intentionally NOT refreshed — original keeps EAX=0. */
    }

    /* Listing 0x0042EF4E: MOV ESI, [ESP+0x14]  (param_2 == phys table). */

    /* Listing 0x0042EF52: MOVSX EBX, [ESI + EAX*2 + 0x3E]  upshift threshold. */
    int32_t up_thresh = (int32_t)PHYS_S(actor, PHYS_GEAR_UPSHIFT_BASE + (int32_t)gear_cached * 2);

    /* Listing 0x0042EF57-70:
     *   CMP EDI,EBX     ; JLE 0x42EFF1  (rpm <= up_thresh → downshift branch)
     *   CMP EAX,0x8     ; JGE 0x42EFF1  (gear_cached >= 8 signed → downshift)
     *   MOV EBX,[ECX+0x314] ; TEST EBX,EBX ; JLE 0x42EFF1  (long_spd<=0 → ds) */
    if (rpm > up_thresh
        && (int32_t)gear_cached < 8
        && actor->longitudinal_speed > 0) {

        /* Listing 0x0042EF72-7A: MOV BL,[ECX+0x36B] ; INC BL ; MOV
         * [ECX+0x36B],BL.  BL is re-loaded fresh (will be 2 after promo). */
        uint8_t new_gear = (uint8_t)(actor->current_gear + 1);
        actor->current_gear = new_gear;

        /* Listing 0x0042EF80-8D:
         *   MOVSX EAX,[ESI+0x68]         ; phys[0x68] (drive_torque_mult)
         *   IMUL EAX,EDX                 ; * throttle
         *   LEA EDX,[EAX+EAX*2]          ; EDX = EAX * 3
         *   LEA EAX,[EAX+EDX*4]          ; EAX = EAX + 12*EAX = 13*EAX
         *   SHL EAX,1                    ; EAX = 26*EAX = EAX*0x1A
         *
         * Listing 0x0042EF8F-A4: CDQ ; AND EDX,0xFF ; ADD EAX,EDX ; SAR EAX,8
         *   → first sar8_rz (truncated divide by 256). */
        int32_t k = (int32_t)PHYS_S(actor, PHYS_DRIVE_TORQUE_MULT) * throttle * 0x1A;
        k = sar8_rz_42EF10(k);

        /* Listing 0x0042EFA7-AD: AND EBX,0xFF ; IMUL EAX,[EBX*4 + 0x467394]
         * EBX still has new_gear in BL from the store at 0x0042EF7A; the
         * AND masks the high bytes (leftover from the 0x314 long_spd load).
         * Table at DAT_00467394 = {0, 0, 0x100, 0xC0, 0x80, 0x40, 0x20,
         * 0x10, 0} (9 dwords). new_gear is bounded by the < 8 gate to
         * [1..8], so indices 1..8 of the table are all that's reachable. */
        static const int32_t g_gear_torque_table[9] = {
            0, 0, 0x100, 0xC0, 0x80, 0x40, 0x20, 0x10, 0
        };
        k = k * g_gear_torque_table[new_gear & 0xFFu];

        /* Listing 0x0042EFB5-CA: MOV EBX,[ECX+0x2EC] ; CDQ ; AND EDX,0xFF ;
         * ADD EAX,EDX ; MOV EDX,[ECX+0x2F8] ; SAR EAX,8
         *   → second sar8_rz. */
        k = sar8_rz_42EF10(k);

        /* Listing 0x0042EFCD-E9: spread k across the four spring-dv slots.
         *   ADD EDI,EAX ; SUB ESI,EAX ; ADD EBX,EAX ; SUB EDX,EAX
         * with EDI=+0x2F0, ESI=+0x2F4, EBX=+0x2EC, EDX=+0x2F8 ─ so:
         *   +0x2EC (FL) += k ; +0x2F0 (FR) += k
         *   +0x2F4 (RL) -= k ; +0x2F8 (RR) -= k */
        actor->wheel_spring_dv[0] += k;   /* +0x2EC FL */
        actor->wheel_spring_dv[1] += k;   /* +0x2F0 FR */
        actor->wheel_spring_dv[2] -= k;   /* +0x2F4 RL */
        actor->wheel_spring_dv[3] -= k;   /* +0x2F8 RR */
        return;
    }

    /* Listing 0x0042EFF1: MOVSX EDX,[ESI + EAX*2 + 0x4E]  dn_thresh
     *                        (still indexed by CACHED gear, not refreshed). */
    int32_t dn_thresh = (int32_t)PHYS_S(actor, PHYS_GEAR_DOWNSHIFT_BASE + (int32_t)gear_cached * 2);

    /* Listing 0x0042EFF6-FF:
     *   CMP EDI,EDX  ; JGE 0x42F005   (rpm >= dn_thresh → skip)
     *   CMP EAX,0x2  ; JLE 0x42F005   (gear_cached <= 2 → skip)
     *   DEC [ECX+0x36B] */
    if (rpm < dn_thresh && (int32_t)gear_cached > 2) {
        actor->current_gear = (uint8_t)(actor->current_gear - 1);
    }
}

/* --- On-ground variant of auto_gear_select ---
 * Same gear logic but WITHOUT the drivetrain kick (wheel_spring_dv writes).
 * The original only calls auto_gear on airborne frames where kicks are rare.
 * The port calls it on-ground every tick, so the kick accumulates and
 * pitches the car up without recovery. */
void td5_physics_auto_gear_select_no_kick(TD5_Actor *actor)
{
    int16_t *phys = get_phys(actor);
    if (!phys) return;

    int32_t throttle    = (int32_t)actor->encounter_steering_cmd;
    uint8_t gear_cached = actor->current_gear;
    int32_t rpm         = actor->engine_speed_accum;

    if (throttle < 0) {
        actor->current_gear = TD5_GEAR_REVERSE;
        return;
    }

    if (gear_cached == TD5_GEAR_REVERSE) {
        if (throttle <= 0) return;
        actor->current_gear = TD5_GEAR_FIRST;
    }

    int32_t up_thresh = (int32_t)PHYS_S(actor, PHYS_GEAR_UPSHIFT_BASE + gear_cached * 2);

    if (rpm > up_thresh
        && gear_cached < 8
        && actor->longitudinal_speed > 0) {
        uint8_t new_gear = (uint8_t)(actor->current_gear + 1);
        actor->current_gear = new_gear;
        /* No drivetrain kick — on-ground only */
        return;
    }

    int32_t dn_thresh = (int32_t)PHYS_S(actor, PHYS_GEAR_DOWNSHIFT_BASE + gear_cached * 2);
    if (rpm < dn_thresh && gear_cached > TD5_GEAR_FIRST) {
        actor->current_gear = (uint8_t)(actor->current_gear - 1);
    }
}

/* Round-to-zero signed divide by 256 — byte-exact port of the 0x0042F030
 * idiom: CDQ ; AND EDX, 0xFF ; ADD EAX, EDX ; SAR EAX, 8.
 *
 *   For x >= 0: plain SAR by 8 (low byte truncated).
 *   For x <  0 with (x & 0xFF) != 0: (x + 0xFF) >> 8 — one unit closer to zero
 *     than plain SAR (truncated division by 256).
 *
 * Equivalent to C99 truncated division: (int32_t)(x / 256). */
static inline int32_t sar8_rz_42F030(int32_t x) {
    return ((x < 0) ? (x + 0xFF) : x) >> 8;
}

/* Round-to-zero signed divide by 512 — original idiom:
 *   CDQ ; AND EDX, 0x1FF ; ADD EAX, EDX ; SAR EAX, 9.
 * Equivalent to C99 truncated division: (int32_t)(x / 512). */
static inline int32_t sar9_rz_42F030(int32_t x) {
    return ((x < 0) ? (x + 0x1FF) : x) >> 9;
}

/* [CONFIRMED @ 0x0042F030] Byte-faithful with orig ComputeDriveTorqueFromGearCurve.
 * L5 promotion 2026-05-18 (small-tier sweep). Piecewise-linear LUT lerp at
 * tuning+rpm_idx*2 (SAR-9 index, mod-512 fraction), * throttle SAR-8 *
 * gear_ratio SAR-8; redline-50 upper cutoff returns 0; gear==1 early-out.
 *
 * --- ComputeDriveTorqueFromGearCurve (0x42F030) ---
 *
 * Byte-exact port from listing 0x0042F030..0x0042F0FC.
 *
 * Piecewise-linear torque curve interpolation.
 *
 *   actor + 0x310  engine_speed_accum  int32
 *   actor + 0x33E  encounter_steering  int16 (signed throttle)
 *   actor + 0x36B  current_gear        uint8 — 0x01 == neutral, early-out
 *   tuning + 0x00..0x1F  LUT[N] int16 (per-512-rpm torque samples)
 *   tuning + 0x2E + gear*2  gear_ratio[gear] int16
 *   tuning + 0x68  torque_mult int16
 *   tuning + 0x72  redline     int16; cutoff when engine_speed > redline-50
 *
 * Audit: re/analysis/pilot_0042F030_audit.md
 */
/* [gear-1 accel 2026-06-18] Speed-dependent drive-torque shaping for the HUMAN
 * player (user: "make acceleration faster at low speed and weaker at high speed,
 * mimic gear 1" — which also strengthens hill starts). Returns a Q8 multiplier:
 * BOOST at/below LOW% of the car's top speed, tapering linearly to CUT at/above
 * HIGH%. 0x100 (unchanged) when off or top speed is unknown. Player-only so AI
 * pacing is untouched. Knobs:
 *   TD5RE_GEAR1_ACCEL=0      disable (default on)
 *   TD5RE_GEAR1_BOOST_PCT    low-speed torque %  (default 150 = 1.5x)
 *   TD5RE_GEAR1_CUT_PCT      high-speed torque % (default 70  = 0.7x)
 *   TD5RE_GEAR1_LOW_PCT      boost holds below this % of top speed (default 12)
 *   TD5RE_GEAR1_HIGH_PCT     cut reached at/above this % of top speed (default 75) */
static int32_t td5_physics_gear1_accel_q8(TD5_Actor *actor)
{
    static int inited = 0, on = 1, boost = 150, cut = 70, lowp = 12, highp = 75;
    int32_t top, spd, lo, hi, bq, cq;
    if (!inited) {
        const char *e;
        inited = 1;
        on = td5_env_flag_on("TD5RE_GEAR1_ACCEL");
        e = getenv("TD5RE_GEAR1_BOOST_PCT"); if (e && e[0]) boost = atoi(e);
        e = getenv("TD5RE_GEAR1_CUT_PCT");   if (e && e[0]) cut   = atoi(e);
        e = getenv("TD5RE_GEAR1_LOW_PCT");   if (e && e[0]) lowp  = atoi(e);
        e = getenv("TD5RE_GEAR1_HIGH_PCT");  if (e && e[0]) highp = atoi(e);
        if (boost < 100) boost = 100; else if (boost > 400) boost = 400;
        if (cut   < 20)  cut   = 20;  else if (cut   > 100) cut   = 100;
        if (lowp  < 0)   lowp  = 0;   else if (lowp  > 90)  lowp  = 90;
        if (highp <= lowp) highp = lowp + 1; else if (highp > 100) highp = 100;
        TD5_LOG_I(LOG_TAG, "gear1 accel: %s boost=%d%% cut=%d%% low=%d%% high=%d%% "
                  "(TD5RE_GEAR1_*)", on ? "ON" : "OFF", boost, cut, lowp, highp);
    }
    if (!on) return 0x100;
    top = (int32_t)PHYS_S(actor, PHYS_TOP_SPEED);
    if (top <= 0) return 0x100;
    spd = actor->longitudinal_speed;
    if (spd < 0) spd = -spd;                              /* 24.8 magnitude */
    lo = (int32_t)((((int64_t)top * lowp)  / 100) << 8);  /* top is in HUD units */
    hi = (int32_t)((((int64_t)top * highp) / 100) << 8);
    bq = (boost * 0x100) / 100;
    cq = (cut   * 0x100) / 100;
    if (spd <= lo) return bq;
    if (spd >= hi || hi <= lo) return cq;
    return bq + (int32_t)(((int64_t)(cq - bq) * (spd - lo)) / (hi - lo));
}

int32_t td5_physics_compute_drive_torque(TD5_Actor *actor)
{
    /* Entry trace hook (pure-leaf function; no state to snapshot at exit). */

    int16_t *phys = get_phys(actor);
    if (!phys) {
        return 0;
    }

    uint8_t  gear_u8 = actor->current_gear;
    int32_t  gear    = (int32_t)gear_u8;

    /* Neutral (gear == 1) — original CMP BL,0x1 / JZ RET_ZERO at 0x0042F03B-45. */
    if (gear_u8 == 0x01) {
        return 0;
    }

    int32_t rpm = actor->engine_speed_accum;

    /* index = sar9_rz(rpm) — listing 0x0042F04B-58 (CDQ/AND 0x1FF/ADD/SAR 9).
     * NO bounds clamp (original reads LUT[index] and LUT[index+1] freely). */
    int32_t index = sar9_rz_42F030(rpm);

    int32_t torque_mult = (int32_t)PHYS_S(actor, PHYS_DRIVE_TORQUE_MULT);

    /* t0 = sar8_rz(LUT[index] * mult) — listing 0x0042F060-72.
     * t1 = sar8_rz(LUT[index+1] * mult) — listing 0x0042F077-8F. */
    int32_t lut_i  = (int32_t)PHYS_S(actor, index * 2);
    int32_t lut_i1 = (int32_t)PHYS_S(actor, index * 2 + 2);
    int32_t t0 = sar8_rz_42F030(lut_i  * torque_mult);
    int32_t t1 = sar8_rz_42F030(lut_i1 * torque_mult);

    /* Signed low-9-bit fraction (truncated-divide remainder mod 512).
     * Listing 0x0042F096-A5: AND ECX,0x800001FF; if neg: DEC; OR 0xFFFFFE00; INC.
     * Algebraically equivalent to C99 truncated remainder rpm % 512.
     * For rpm >= 0: frac in [0, 511].
     * For rpm <  0: frac in [-511, 0]. */
    int32_t frac = rpm % 512;

    /* lerp = sar9_rz((t1 - t0) * frac) + t0 — listing 0x0042F0A6-C0. */
    int32_t torque = sar9_rz_42F030((t1 - t0) * frac) + t0;

    /* * throttle — original at 0x0042F0C2-D0 uses sar8_rz. Throttle is signed
     * (encounter_steering_cmd, sign-flipped by 0x42F010 for reverse gear). */
    int32_t throttle = (int32_t)actor->encounter_steering_cmd;
    torque = sar8_rz_42F030(torque * throttle);

    /* * gear_ratio[gear] — original at 0x0042F0D2-EF uses sar8_rz on the
     * EBX-byte-masked gear index. PHYS_S already does 16-bit signed load. */
    int32_t gear_ratio = (int32_t)PHYS_S(actor, PHYS_GEAR_RATIO_BASE + (int32_t)(uint8_t)gear_u8 * 2);
    torque = sar8_rz_42F030(torque * gear_ratio);

    /* Redline cutoff: original computes the full pipeline then CMPs EBP (raw
     * engine_speed) against ECX (redline-50). JLE keep result; else XOR=0.
     * The compare is signed (JLE), so rpm > redline-50 → return 0. */
    int32_t redline = (int32_t)PHYS_S(actor, PHYS_REDLINE_RPM);
    if (rpm > redline - 50) {
        return 0;
    }

    /* [MP CATCHUP 2026-06-14] Multiplayer rubber-band assist: in a 2+-human
     * race, scale a HUMAN player's longitudinal drive force by the per-slot
     * catch-up multiplier (Q8, 0x100 = 1.0). Behind the leader -> >1.0 (catch
     * up); leader (when enabled) -> <1.0 (ease off). The multiplier is 1.0 for
     * AI/traffic, for non-racer slots, and whenever the feature is off or there
     * is <2 humans, so this is a no-op in single-player and faithful play.
     *
     * Applied here (the single drive-torque chokepoint feeding all three player
     * drive paths: on-ground / airborne / reverse) so the assist is consistent
     * and smooth. It only scales ACCELERATION — top speed (the speed_limit gate
     * in the callers) is untouched, so it cannot warp a car past the field; it
     * just lets a trailing player build speed harder. The shift uses the same
     * biased-toward-zero signed >>8 idiom as the rest of this pipeline.
     *
     * DETERMINISM: gated on g_race_slot_state[] + s_mp_catchup_mult[], both of
     * which are pure replicated sim state recomputed once per tick — identical
     * on every lockstep client, so torque stays bit-identical across machines. */
    if (actor->slot_index < g_traffic_slot_base &&
        actor->slot_index < TD5_MAX_RACER_SLOTS &&
        g_race_slot_state[actor->slot_index] == 1) {
        int32_t mult = td5_physics_mp_catchup_mult(actor->slot_index);
        if (mult != MP_CATCHUP_Q8_ONE) {
            int64_t scaled = (int64_t)torque * (int64_t)mult;
            /* signed /256, biased toward zero (matches sar8_rz_42F030 etc.) */
            torque = (int32_t)((scaled + ((scaled >> 63) & 0xFF)) >> 8);
        }
    }

    /* [ARCADE NITRO 2026-06-27] While a racer is boosting (NITRO power-up active)
     * scale its drive force by the arcade acceleration multiplier (default 2.5x).
     * Applies to ANY racer slot that grabbed a NITRO box (human OR AI), so it is
     * NOT gated on g_race_slot_state==1 like the MP block above — just on being a
     * racer slot. ACCELERATION only: the per-car top-speed gate in the callers is
     * untouched, so a boosting car builds speed harder but is not warped past its
     * cap. 1.0 (no-op) outside arcade mode / when NITRO is inactive. Same
     * biased-toward-zero signed >>8 idiom; same lockstep-deterministic basis. */
    if (actor->slot_index >= 0 &&
        actor->slot_index < g_traffic_slot_base &&
        actor->slot_index < TD5_MAX_RACER_SLOTS) {
        int32_t nmult = td5_arcade_slot_accel_q8((int)actor->slot_index);
        if (nmult != 0x100) {
            int64_t scaled = (int64_t)torque * (int64_t)nmult;
            torque = (int32_t)((scaled + ((scaled >> 63) & 0xFF)) >> 8);
        }
    }

    /* [HARD CATCHUP item #13] Hard-difficulty AI catch-up: the complement of the
     * MP block above — scale an AI OPPONENT's drive force up when it is behind
     * the human player on Hard, so the field presses harder. Applies ONLY to AI
     * racer slots (non-human, slot < g_traffic_slot_base); humans are handled
     * above and traffic/non-racer slots return 1.0. The multiplier is 1.0 (no-op)
     * unless TD5RE_HARD_CATCHUP=1 AND g_difficulty_hard, so non-hard / easy /
     * normal play is byte-unchanged. Acceleration only (top-speed gate in the
     * callers untouched). Same biased-toward-zero signed >>8 idiom.
     *
     * DETERMINISM: gated on g_race_slot_state[] + g_difficulty_hard +
     * s_hard_catchup_mult[], all replicated sim state recomputed once per tick. */
    if (actor->slot_index >= 0 &&
        actor->slot_index < g_traffic_slot_base &&
        actor->slot_index < TD5_MAX_RACER_SLOTS &&
        g_race_slot_state[actor->slot_index] != 1) {
        int32_t mult = td5_physics_hard_catchup_mult(actor->slot_index);
        if (mult != MP_CATCHUP_Q8_ONE) {
            int64_t scaled = (int64_t)torque * (int64_t)mult;
            torque = (int32_t)((scaled + ((scaled >> 63) & 0xFF)) >> 8);
        }
    }

    /* [MANUAL BOOST #2] Acceleration half: a car in MANUAL gearbox mode (byte
     * +0x378 == 0) gets +N% drive torque (default +20%). Applies to ALL cars
     * (player / AI / traffic) since it keys only on the per-actor gearbox byte.
     * The top-speed half lives at the speed_limit gates in the callers. 1.0 (no
     * boost) for automatic cars or when the knob is off, so they are unchanged.
     * Same biased-toward-zero signed >>8 idiom as the catch-up blocks above. */
    {
        int32_t mboost = td5_physics_actor_manual_boost_q8(actor);
        if (mboost != MP_CATCHUP_Q8_ONE) {
            int64_t scaled = (int64_t)torque * (int64_t)mboost;
            torque = (int32_t)((scaled + ((scaled >> 63) & 0xFF)) >> 8);
        }
    }

    /* [gear-1 accel #2026-06-18] HUMAN-player-only speed shaping: strong low-end,
     * tapered top-end (mimics gear 1; also helps hill starts). Composes after the
     * catch-up/manual multipliers. g_race_slot_state==1 is the human-driven slot
     * (AI racers / traffic are untouched, preserving pacing). */
    if (actor->slot_index >= 0 && actor->slot_index < TD5_MAX_RACER_SLOTS &&
        g_race_slot_state[actor->slot_index] == 1) {
        int32_t g1 = td5_physics_gear1_accel_q8(actor);
        if (g1 != 0x100) {
            int64_t scaled = (int64_t)torque * (int64_t)g1;
            torque = (int32_t)((scaled + ((scaled >> 63) & 0xFF)) >> 8);
        }
    }

    /* [WEIGHT power-to-weight 2026-06-25] Lighter cars accelerate harder, heavier
     * cars build speed more slowly — RACER slots only (player + AI); traffic keeps
     * its own dynamics. Q8 from collision_mass via the shared heaviness math, so
     * the ACCEL bar on the car-select screen matches the feel. Composes after the
     * catch-up/manual/gear-1 multipliers; same biased-toward-zero >>8 idiom.
     * Knob TD5RE_WEIGHT_ACCEL_PCT (master TD5RE_WEIGHT_MECH). */
    if (actor->slot_index >= 0 && actor->slot_index < g_traffic_slot_base) {
        int32_t pw = td5_physics_weight_accel_q8(actor);
        if (pw != 0x100) {
            int64_t scaled = (int64_t)torque * (int64_t)pw;
            torque = (int32_t)((scaled + ((scaled >> 63) & 0xFF)) >> 8);
        }
    }

    /* [SLIPSTREAM 2026-06-25] Forward drive boost when running in another car's
     * wake (per-slot Q8 computed by td5_physics_update_draft once this tick).
     * Racer slots only; 1.0 when no car is ahead. Knob TD5RE_DRAFT / _PCT. */
    if (actor->slot_index >= 0 && actor->slot_index < g_traffic_slot_base) {
        int32_t df = td5_physics_draft_mult((int)actor->slot_index);
        if (df != 0x100) {
            int64_t scaled = (int64_t)torque * (int64_t)df;
            torque = (int32_t)((scaled + ((scaled >> 63) & 0xFF)) >> 8);
        }
    }

    return torque;
}

/* [CONFIRMED @ 0x0042EEA0] Byte-faithful with orig ApplySteeringTorqueToWheels.
 * L5 promotion 2026-05-18 (small-tier sweep). Verbatim port of 24-instr listing;
 * throttle * tuning[+0x68] * 26, biased SAR 8, * g_gearTorqueTable[gear],
 * biased SAR 8 → kick written FL/FR (+), RL/RR (-) on wheel_spring_dv[0..3].
 *
 * --- ApplySteeringTorqueToWheels (0x42EEA0) ---  [byte-faithful port]
 *
 * Verbatim port of FUN_0042EEA0 (0x0042EEA0..0x0042EF06). Originally the
 * port stubbed this out to suppress a pitch-divergence symptom, but the
 * batch precise-port mandate is byte-faithful behaviour first; downstream
 * suspension fixes belong in their owning functions. Disassembly:
 *
 *   MOVSX EDX,[param2+0x68]            ; cardef/tuning short (drive-torque mult)
 *   MOVSX EAX,[param1+0x33E]           ; actor encounter_steering_cmd (s16)
 *   IMUL  EAX,EDX                      ; throttle * mult
 *   LEA   EDX,[EAX+EAX*2]              ; *3
 *   LEA   EAX,[EAX+EDX*4]              ; *(1+12) = *13
 *   SHL   EAX,1                        ; *26 (= 0x1A)
 *   CDQ / AND EDX,0xff / ADD EAX,EDX / SAR EAX,0x8   ; biased >>8
 *   XOR   EDX,EDX / MOV DL,[param1+0x36B]            ; zero-ext current_gear
 *   IMUL  EAX,[EDX*4 + 0x467394]       ; g_gearTorqueTable[gear]
 *   CDQ / AND EDX,0xff / ADD EAX,EDX / SAR EAX,0x8   ; biased >>8
 *   ADD   [param1+0x2EC],EAX           ; wheel_spring_dv[0] += k (FL)
 *   ADD   [param1+0x2F0],EAX           ; wheel_spring_dv[1] += k (FR)
 *   SUB   [param1+0x2F4],EAX           ; wheel_spring_dv[2] -= k (RL)
 *   SUB   [param1+0x2F8],EAX           ; wheel_spring_dv[3] -= k (RR)
 *
 * The (x + ((x>>31)&0xff)) >> 8 idiom is the biased-toward-zero arithmetic
 * right-shift Microsoft's compiler emits for signed /256. g_gearTorqueTable
 * at DAT_00467394 mirrors the same LUT used inline by auto_gear_select.
 */
void td5_physics_apply_steering_torque(TD5_Actor *actor)
{
    int16_t *phys = get_phys(actor);
    if (!phys) return;

    /* DAT_00467394 — g_gearTorqueTable (int32[]). Indexed by current_gear
     * read as an unsigned byte; only entries 0..8 are meaningful (0,0,
     * 256,192,128,64,32,16,0).
     *
     * BOUNDS AUDIT (2026-05-14, fix-gear-bounds):
     *   Every writer to actor+0x36B in TD5_d3d.exe is bounded to [0..8]:
     *     0x40368e  player INC, gated by  gear < gear_count - 1
     *     0x4036xx  player DEC, gated by  gear != 0
     *     0x405dbf  ResetVehicleActorState   = 0x02 (constant)
     *     0x42ef32  UpdateAutomaticGearSelection = 0x00 (constant)
     *     0x42ef47  UpdateAutomaticGearSelection = 0x02 (constant)
     *     0x42ef7a  UpdateAutomaticGearSelection INC, gated by gear < 8
     *     0x42efff  UpdateAutomaticGearSelection DEC, gated by gear > 2
     *   Port-side writers in td5_physics.c match those bounds line-for-line
     *   (see td5_physics_auto_gear_select / _no_kick — INC under `< 8` gate,
     *   DEC under `> 2` gate, plus four constant-only writes of 0 / 2).
     *
     *   Reachable indices in normal play: 2..7 (active forward gears) + 0
     *   (reverse) + 8 (transient one-tick upshift result of the < 8 gate
     *   when current_gear == 7).  Indices 1 and 9..255 are unreachable.
     *
     * The 256-entry zero-filled expansion below is DEFENSIVE belt-and-
     * suspenders against any future writer that violates the [0..8] range.
     * It is NOT required for byte-faithful behavior — the original's nine-
     * entry table is reached at indices 0..8 only, and the port respects
     * that. The expansion costs 988 bytes of .rodata to make an OOB read
     * silent (returns 0 → no kick) rather than crashing on a malformed
     * save or modded actor stream. */
    static const int32_t g_gear_torque_table[256] = {
        [2] = 0x100,
        [3] = 0xC0,
        [4] = 0x80,
        [5] = 0x40,
        [6] = 0x20,
        [7] = 0x10,
        /* indices 0,1,8..255 = 0 (default) */
    };

    int32_t throttle    = (int32_t)actor->encounter_steering_cmd;  /* +0x33E s16 */
    int32_t torque_mult = (int32_t)PHYS_S(actor, PHYS_DRIVE_TORQUE_MULT);            /* tuning +0x68 s16 */

    int32_t k = throttle * torque_mult * 0x1A;
    k = (k + ((k >> 31) & 0xff)) >> 8;
    k = k * g_gear_torque_table[(uint8_t)actor->current_gear];     /* +0x36B u8 */
    k = (k + ((k >> 31) & 0xff)) >> 8;

    actor->wheel_spring_dv[0] += k;   /* +0x2EC FL */
    actor->wheel_spring_dv[1] += k;   /* +0x2F0 FR */
    actor->wheel_spring_dv[2] -= k;   /* +0x2F4 RL */
    actor->wheel_spring_dv[3] -= k;   /* +0x2F8 RR */
}

/* [CONFIRMED @ 0x0042F010] Byte-faithful with orig ApplyReverseGearThrottleSign.
 * L5 promotion 2026-05-18 (small-tier sweep). 8-instr listing match;
 * actor+0x36B (gear) zero-gate, 16-bit NEG on actor+0x33E (encounter_steering_cmd).
 *
 * --- ApplyReverseGearThrottleSign (0x42F010) ---
 *
 * Byte-exact port from listing 0x0042F010..0x0042F02F (8 instructions).
 *
 *   0042f010  MOV  EAX,dword ptr [ESP + 0x4]      ; actor
 *   0042f014  MOV  CL,byte ptr [EAX + 0x36b]      ; gear_u8 = actor->current_gear
 *   0042f01a  TEST CL,CL
 *   0042f01c  JNZ  0x0042f02f                     ; if gear != 0, skip
 *   0042f01e  MOV  CX,word ptr [EAX + 0x33e]      ; thr = actor->encounter_steering_cmd
 *   0042f025  NEG  CX                             ; thr = -thr (16-bit two's complement)
 *   0042f028  MOV  word ptr [EAX + 0x33e],CX
 *   0042f02f  RET
 *
 * Flips the signed throttle term in-place when current_gear == 0 (REVERSE) so
 * the same forward drive-torque pipeline (0x0042F030) can be reused for
 * backward motion. Field +0x36B is the 1-byte current_gear (REVERSE=0,
 * NEUTRAL=1, FIRST=2, ...). Field +0x33E is the int16_t encounter_steering_cmd
 * (also reachable as the signed-throttle source consumed by
 * ComputeDriveTorqueFromGearCurve). 16-bit NEG matches C int16_t negation. */
void td5_physics_reverse_throttle_sign(TD5_Actor *actor)
{
    /* TEST CL,CL / JNZ — early-out when gear != REVERSE (0). */
    if (actor->current_gear != 0)
        return;

    /* NEG CX on a 16-bit word in memory — int16_t two's-complement negation. */
    actor->encounter_steering_cmd = (int16_t)-(int32_t)actor->encounter_steering_cmd;
}

/* --- ComputeReverseGearTorque (0x00403C80) — byte-faithful port ---
 *
 * Despite the Ghidra name, this function does NOT compute torque. It
 * (a) produces the RPM-encoded pseudo-speed written back as the caller's
 * longitudinal_speed or lateral_speed, and (b) slews engine_speed_accum
 * toward a target that depends on throttle, gear, brake, and a caller-
 * supplied signed speed term. It is the GROUND-PATH authoritative engine
 * updater — UpdateEngineSpeedAccumulator (UESA) runs on the airborne path
 * instead; the two are mutually exclusive per tick.
 *
 * Byte-exact port from listing 0x00403C80..0x00403D82 (98 instructions).
 *
 * Original signature (Ghidra): __cdecl(phys *param_1, actor *param_2, int speed_in)
 *   param_1 (EAX,[ESP+0xC])  phys/tuning ptr   — reads [+0x2E + gear*2], [+0x72]
 *   param_2 (EDI,[ESP+0x18]) actor ptr         — reads [+0x310],[+0x33E],[+0x36B],[+0x36D]
 *                                                writes [+0x310]
 *   param_3      ([ESP+0x20]) signed speed_in
 *
 * Port keeps the existing (actor, speed_in) wrapper signature; `get_phys`
 * recovers param_1.  All math/control-flow inside is line-for-line with
 * the original.
 *
 * Original logic:
 *   gear == 1  (neutral) → iVar5 (ret_value) = 0, BUT continues to slew
 *                          engine through the cold branch (target=0).
 *   gear != 1            → iVar5 = ((((rpm-400)*0x1000) /45 trunc) /gear_ratio
 *                                  trunc) << 8
 *
 *   throttle  > 0 AND gear == 2 → hot branch:
 *      u = sar8_rz(speed_in * 4); if (u<0) u = 0  (SETS/DEC/AND clamp)
 *      target = u + redline - 0x708 ;  step = 200
 *   else                          → cold branch:
 *      target = 0; step = (brake ? 400 : 200) * 2  (i.e. 800 or 400)
 *
 *   Slew engine_speed_accum toward target:
 *     if (target < engine):                              # descending (JLE label)
 *         engine -= step
 *         if (engine < target):  write target (clamp)
 *         else:                  write engine (stepped)
 *     else:                                              # ascending
 *         if (engine < target - 4*step):  write engine + step  (big-gap ramp)
 *         else:  engine += sar2_rz(target - engine); write engine  (exp pull)
 *
 *   Return iVar5 (the encoded pseudo-speed).
 *
 * sar8_rz / sar2_rz : C99 truncated signed division by 256 / 4. */
static inline int32_t sar8_rz_403C80(int32_t x) {
    /* CDQ ; AND EDX,0xFF ; ADD EAX,EDX ; SAR EAX,8.
     * Equivalent to (int32_t)(x / 256) — truncate toward zero. */
    return ((x < 0) ? (x + 0xFF) : x) >> 8;
}
static inline int32_t sar2_rz_403C80(int32_t x) {
    /* CDQ ; AND EDX,3 ; ADD EAX,EDX ; SAR EAX,2.
     * Equivalent to (int32_t)(x / 4). */
    return ((x < 0) ? (x + 3) : x) >> 2;
}

int32_t compute_reverse_gear_torque(TD5_Actor *actor, int32_t speed_in)
{
    if (!actor) return 0;
    int16_t *phys = get_phys(actor);
    if (!phys) return 0;
    (void)phys;  /* PHYS_S(actor, off) wraps the same dereference. */

    /* Stack-mirroring the listing for clarity:
     *   ECX = engine          (rpm in/out)
     *   EBX = gear            (BL, byte-extended)
     *   EDX = redline (saved at [ESP+0x18])
     *   EBP = step (200 default, kept at [ESP+0x10])
     *   ESI = iVar5  (return value)
     *   EAX = target  (after target-build joins)  */
    int32_t engine  = actor->engine_speed_accum;
    int32_t gear    = (int32_t)actor->current_gear;          /* zero-extended byte */
    int32_t redline = (int32_t)PHYS_S(actor, PHYS_REDLINE_RPM);          /* MOVSX from [+0x72] */
    int32_t step    = 200;                                    /* MOV EBP,0xC8 */

    /* --- Encode pseudo-speed (iVar5 / ESI) --- */
    int32_t iVar5;
    if (gear == 1) {
        /* 0x00403CAF JNZ skip → XOR ESI,ESI ; JMP 0x00403CE4.
         * Neutral SKIPS the encode but does NOT early-return; engine slew
         * still runs through the cold branch below. */
        iVar5 = 0;
    } else {
        int32_t gear_ratio = (int32_t)PHYS_S(actor, PHYS_GEAR_RATIO_BASE + gear * 2);

        /* LEA ESI,[ECX-400] ; SHL ESI,12  →  ESI = (engine - 400) * 0x1000.
         * Then 0xB60B60B7 IMUL idiom = ESI / 45 with truncate-toward-zero
         * via SAR 5 + sign-bit fixup (SHR 31 / ADD).
         * Then CDQ / IDIV gear_ratio truncates toward zero. */
        int32_t num = (engine - 400) * 0x1000;
        int32_t div45 = num / 45;                            /* C99 trunc */
        int32_t div_gr = div45 / gear_ratio;                 /* C99 trunc */
        iVar5 = div_gr << 8;
    }

    /* --- Target + step selection (0x00403CE4..0x00403D2F) --- */
    int32_t throttle = (int32_t)actor->encounter_steering_cmd;  /* MOVSX [+0x33E] */
    int32_t target;

    if (throttle > 0 && gear == 2) {
        /* Hot branch 0x00403CF3..0x00403D1C:
         *   EAX = speed_in*4 ; sar8_rz ; clamp <0 → 0 ; + redline - 0x708. */
        int32_t u = sar8_rz_403C80(speed_in * 4);
        /* SETS DL ; DEC EDX ; AND EAX,EDX → clamp u<0 to 0. */
        if (u < 0) u = 0;
        target = u + redline - 0x708;
        /* step stays at 200 (EBP unmodified on this path). */
    } else {
        /* Cold branch 0x00403D1E..0x00403D2F:
         *   if (brake_flag != 0) EBP = 0x190 (400)
         *   ADD EBP,EBP            → EBP *= 2  → 400 or 800
         *   XOR EAX,EAX            → target = 0 */
        if (actor->brake_flag != 0) {
            step = 0x190;        /* 400 */
        }
        step = step + step;      /* 400 or 800 */
        target = 0;
    }

    /* --- Slew engine_speed_accum (0x00403D31..0x00403D75) --- */
    /* CMP ECX,EAX ; JLE ascending. Note: JLE in original is SIGNED-LE.
     * The original CMP is engine (ECX) vs target (EAX). JLE means
     * engine <= target → take ascending branch. We invert: descending
     * iff engine > target. */
    if (engine > target) {
        /* Descending 0x00403D35..0x00403D48:
         *   SUB ECX,EBP        (engine -= step)
         *   CMP ECX,EAX ; JGE write_dec
         *   else: [+0x310] = EAX (clamp to target)   ; return ESI
         *   write_dec: [+0x310] = ECX                ; return ESI */
        engine = engine - step;
        if (engine >= target) {
            actor->engine_speed_accum = engine;       /* write_dec / 0x00403D75 */
        } else {
            actor->engine_speed_accum = target;       /* clamp / 0x00403D3B */
        }
    } else {
        /* Ascending 0x00403D49..0x00403D75:
         *   EDX = step*4 ; EBX = target - step*4
         *   CMP ECX,EBX ; JGE exp_pull
         *   else: ECX += EBP ; [+0x310] = ECX        ; return ESI  (big-gap ramp)
         *   exp_pull: EAX = target - engine ; sar2_rz ; ECX += EAX
         *            [+0x310] = ECX                  ; return ESI */
        int32_t threshold = target - step * 4;
        if (engine < threshold) {
            actor->engine_speed_accum = engine + step;  /* big-gap / 0x00403D5A */
        } else {
            int32_t delta = target - engine;
            int32_t inc   = sar2_rz_403C80(delta);
            actor->engine_speed_accum = engine + inc;   /* exp pull / 0x00403D75 */
        }
    }

    return iVar5;
}

/* ========================================================================
 * Surface Normal & Gravity -- ComputeVehicleSurfaceNormalAndGravity (0x42EBF0)
 *
 * Computes effective gravity vector projected onto body axes.
 * ======================================================================== */

/* Byte-faithful port of FUN_0042CCD0 "StoreRoundedVector3Ints" — actually
 * NormalizeVec3iToConstantMagnitude4096. The original FPU sequence is:
 *   - FILD each int component → 80-bit (truncated to PC=53 by phase-1 FPU CW)
 *   - sum = x*x + y*y + z*z   (double, FMUL/FADDP on stack)
 *   - scale = 4096.0 / sqrt(sum)  (FSQRT + FDIVR on the float constant)
 *   - foreach component: __ftol(scale * component) writes int (RC=11
 *     truncate-toward-zero via FLDCW then FISTP qword)
 *
 * If sum == 0, the original divides by zero → returns the indefinite integer
 * sentinel. We guard against that explicitly; in practice the four body probes
 * are always distinct, but on tick 0 they can all be zero before init.
 * [CONFIRMED @ 0x0042CCD0 listing 2026-05-14] */
static void td5_normalize_vec3i_to_4096(int32_t v[3]) {
    double x = (double)v[0];
    double y = (double)v[1];
    double z = (double)v[2];
    double sum = x * x + y * y + z * z;
    if (sum <= 0.0) {
        v[0] = 0; v[1] = 0; v[2] = 0;
        return;
    }
    double scale = 4096.0 / sqrt(sum);
    /* C99 cast-to-int truncates toward zero — matches __ftol with RC=11. */
    v[0] = (int32_t)(scale * x);
    v[1] = (int32_t)(scale * y);
    v[2] = (int32_t)(scale * z);
}

void td5_physics_compute_surface_gravity(TD5_Actor *actor)
{
    /* Original @ 0x42EBF0: uses 4 wheel probe world positions to compute
     * two diagonal-difference vectors of the body probes, normalizes each
     * to length 4096, cross-products them into a tilt vector, and projects
     * gravity onto X and Z body axes.
     *
     * Listing reference: 0x0042EBF0..0x0042ED47 (audited 2026-05-14 TD5_pool7).
     *
     * [CONFIRMED @ 0x42EBFA-0x42ECB7: actor offsets +0x090 FL, +0x09C FR,
     *  +0x0A8 RL, +0x0B4 RR; gravity int @ 0x00467380; mag-4096 @ 0x0046736C] */
    TD5_Vec3_Fixed *fl = &actor->probe_FL;
    TD5_Vec3_Fixed *fr = &actor->probe_FR;
    TD5_Vec3_Fixed *rl = &actor->probe_RL;
    TD5_Vec3_Fixed *rr = &actor->probe_RR;

    /* Phase 1 — Diagonal-difference vectors of the 4 body probes.
     * v1 = FL - FR - RR + RL   (per axis, then >> 8)  [SAR @ 0x42ec27 et al]
     * v2 = FL - RR - RL + FR   (per axis, then >> 8)  [SAR @ 0x42ec86 et al]
     * Plain arithmetic SAR — no round-toward-zero idiom here. */
    int32_t v1[3];
    v1[0] = (fl->x - fr->x - rr->x + rl->x) >> 8;
    v1[1] = (fl->y - fr->y - rr->y + rl->y) >> 8;
    v1[2] = (fl->z - fr->z - rr->z + rl->z) >> 8;

    int32_t v2[3];
    v2[0] = (fl->x - rr->x - rl->x + fr->x) >> 8;
    v2[1] = (fl->y - rr->y - rl->y + fr->y) >> 8;
    v2[2] = (fl->z - rr->z - rl->z + fr->z) >> 8;

    /* Pilot trace — pre-normalize snapshot (so the diff can localize Phase 1
     * vs Phase 2 divergence if it ever recurs). Compile-out for release builds. */

    /* Phase 2 — Normalize both diagonals to length 4096.
     * [CONFIRMED @ 0x42ecbf + 0x42ecd0 (CALL 0x42ccd0)] */
    td5_normalize_vec3i_to_4096(v1);
    td5_normalize_vec3i_to_4096(v2);

    /* Phase 3 — CrossProduct3i_FixedPoint12(v1, v2):
     *   cross[0] = (v2.z * v1.y - v1.z * v2.y) >> 12   (= v1.y*v2.z - v1.z*v2.y)
     *   cross[1] = (v1.z * v2.x - v1.x * v2.z) >> 12   (UNUSED — no Y projection)
     *   cross[2] = (v1.x * v2.y - v2.x * v1.y) >> 12
     * [CONFIRMED @ 0x0042EAC0 listing 2026-05-14] */
    int32_t cross_x = (v1[1] * v2[2] - v1[2] * v2[1]) >> 12;
    int32_t cross_z = (v1[0] * v2[1] - v2[0] * v1[1]) >> 12;

    /* Phase 4 — Project gravity onto X and Z body axes.
     * IMUL g, cross_n  → /2 round-toward-zero (matches C99 signed `/2`).
     * Then /4096 with round-toward-zero via `(ax + ((ax>>31)&0xfff)) >> 12`,
     * matching the original CDQ+AND 0xfff+ADD+SAR sequence.
     * [CONFIRMED @ 0x42eceb-0x42ed3b listing 2026-05-14] */
    int32_t ax = (g_gravity_constant * cross_x) / 2;
    int32_t az = (g_gravity_constant * cross_z) / 2;
    actor->linear_velocity_x += (ax + ((ax >> 31) & 0xfff)) >> 12;
    actor->linear_velocity_z += (az + ((az >> 31) & 0xfff)) >> 12;

    /* Pilot trace — post-update snapshot. */
}
