## Pilot Audit — 0x00436A70 UpdateRaceActors

**Date:** 2026-05-14
**Pool slot:** TD5_pool13
**Port-side function:** `td5_ai_update_race_actors` @ `td5_ai.c:3030`
**Worktree:** `.claude/worktrees/precise-00436A70` on branch `precise-00436A70` from `master`
**Tag:** `pool13_00436A70`
**Caller graph:** `RunRaceFrame @ 0x0042B580` (single caller, per sub-tick)
**Callee graph:** ComputeAIRubberBandThrottle (0x00432D60), SinFixed12bit (0x0040A700), CosFixed12bit (0x0040A6E0), `__ftol` (0x0044817C), ComputeTrackSpanProgress (0x004345B0), RenderTrackSegmentNearActor (0x00433CE0), UpdateActorTrackBounds (0x004366E0), ClassifyTrackOffsetClamp (0x004368A0), ComputeSignedTrackOffset (0x00434670), GetTrackSegmentSurfaceType (0x0042F100), UpdateActorTrackBehavior (0x00434FE0), UpdateVehicleActor (0x00406650), UpdateTrafficRoutePlan (0x00435E80), UpdateTrafficActorMotion (0x00443ED0), UpdateSpecialTrafficEncounter (0x00434DA0).
**Body:** 0x00436A70..0x00437091 (0x621 bytes / 450 instructions / ~270 decompiled lines).

## Function structure (from listing)

The original is a single per-tick master dispatcher with FOUR contiguous regions.

### Region A — Rubber-band call (0x00436A77-7B)

```
CALL  0x00432D60  ; ComputeAIRubberBandThrottle  (pool11 pilot — byte-faithful)
```

### Region B — Per-racer track-state loop (0x00436A82..0x00436E86)

Iterates `[ESP+0x10] = local_8` from 0 while `< g_racerCount`. For each slot i:

```
EBX = local_8 = i
EDI = i * 0x388                        ; actor offset
ESI = i * 0x11C                        ; route_state offset

; ── Branch detection (0x436A96..0x436ADB) ──
ECX = g_trackTotalSpanCount = [0x004C3D90]
if (actor[i].span_raw > g_trackTotalSpanCount):       ; +0x80 signed16
    gActorRouteTableSelector[i*0x47] = 1
    gActorRouteStateTable[i*0x47]    = DAT_004B08B4   ; RIGHT.TRK base
    JMP 0x436B43
else:
    ; Walk DAT_004C3DA0 (RIGHT.TRK header) for branch-vs-main classification.
    ; Layout: int32 header_size at +0x14, then triplets at +0x1C (3 words each):
    ;   word[0] = start_span (lo bound)
    ;   word[1] = end_span_excl - start_span  (LEN of branch)
    ;   word[2] = main_remap_offset
    ; For each row j in 0..header_size:
    ;   if (actor.span_norm >= row.start &&
    ;       actor.span_norm <= row.start + (row[-1] - row[-2]) - 1)
    ;     ; the test against `row[-1] - row[-2]` reads the PREVIOUS row's
    ;     ; second/third fields (look-behind) — DO NOT collapse incorrectly.
    ;     ;   ECX (in EAX*0x6 + 0xC) = j*3+0xC  (word index into DAT)
    ;     ;   compare: row.start_offset_from_main + actor.span_norm == -1?
    ;     ;     → break out of branch detection (untouched selector=0)
    ;     ;   else → gActorRouteTableSelector[i] = 0
    ;     ;          gActorRouteStateTable[i]    = DAT_004AFB58  (LEFT.TRK)
    ;     ;          JMP 0x436B43
    ;     ; (if neither condition fires, fall through with selector=0)
    if no branch row matched:
        gActorRouteTableSelector[i*0x47] = 0     ; default to main route

; ── Forward-track-component (0x436B43..0x436BC4) ──
LAB_00436b43:
    span_norm = actor[i].span_normalized            ; +0x82 signed16
    EDX = span_norm * 3
    route_table = gActorRouteStateTable[i*0x47]
    byte route_heading_byte = route_table[span_norm * 3 + 1]
    ; angle_12 = (byte * 0x102C) signed >> 8  (24.8 → 12 bit)
    EAX = byte * 0x102C                              ; signed IMUL
    CDQ ; AND EDX,0xFF ; ADD EAX,EDX ; SAR EAX, 8    ; → round-to-zero /256
    EBP = EAX
    sin_v = SinFixed12bit(EBP)                       ; pool3 pilot
    cos_v = CosFixed12bit(EBP)
    ; sqrt(sin²+cos²)  (always ~4096 ± small LUT error)
    [ESP+0x1C] = sin_v*sin_v + cos_v*cos_v           ; signed product, sum into ECX
    FILD  [ESP+0x1C]   ; FSQRT   ; __ftol           ; → EAX = lrintf(sqrt(...))

    ; numerator = (linvel_z >> 8) * cos_v + (linvel_x >> 8) * sin_v
    EDX = (actor[i].lin_vel_z >> 8) * sin_v          ; actor+0x1D4
    EAX = (actor[i].lin_vel_x >> 8) * cos_v          ; actor+0x1CC
    ; NOTE: orig reads vel_z (+0x1D4) into rot[0x1D4>>2 ... ]; both >>8 SAR.
    EAX += EDX
    CDQ ; IDIV ECX                                   ; signed divide
    gActorForwardTrackComponent[i*0x47] = EAX        ; +0xC0

    ; ── Span progress (0x436BC2..0x436BE2) ──
    lvar = ComputeTrackSpanProgress(actor.span_raw,
                                     &actor.world_pos_x);   ; +0x1FC
    gActorTrackSpanProgress[i*0x47] = (int)lvar      ; +0xC4

    ; ── Render + track-bounds calls (0x436BE2..0x436BFE) ──
    RenderTrackSegmentNearActor(&gActorRouteStateTable[i*0x47]);
    UpdateActorTrackBounds(i);

; ── Offset clamp + recompute (0x436BFE..0x436CEE) ──
    cls = ClassifyTrackOffsetClamp(i, gLateralOffsetBias[i*0x47]);   ; +0x24
    if (cls == 1):
        ; Need to recompute against the LEFT (=−0x20-relative) wall.
        EAX = actor.span_normalized
        EDX = EAX * 3
        route_byte = route_table[EAX*0x1 + EDX]
        signed_offset = ComputeSignedTrackOffset(
                            (int16_t)actor.span_raw, 0x100, route_byte);
        ;   arg2 = 0x100 (request the LEFT extent)
        ; far_pair_ptr = actor.car_def_ptr (0x1B8)
        ; iVar6 = first_short(far_pair_ptr) − 0x20 + signed_offset
        gLateralOffsetBias[i*0x47] = iVar6
        JMP 0x436CEE
    else:
        cls2 = ClassifyTrackOffsetClamp(i, gLateralOffsetBias[i*0x47]);
        if (cls2 == 2):
            EAX = actor.span_normalized
            EDX = EAX * 3
            ECX = route_table[base]                  ; uses EBP=&route_table_ptr
            route_byte = route_table[EAX*0x1 + EDX]
            signed_offset = ComputeSignedTrackOffset(
                                (int16_t)actor.span_raw, 0, route_byte);
            ; arg2 = 0 (RIGHT extent)
            ; near_pair_ptr = actor.car_def_ptr + 8
            ; iVar6 = first_short(near_pair_ptr + 8) + 0x20 + signed_offset
            gLateralOffsetBias[i*0x47] = iVar6

; ── Surface-type override (0x436CEE..0x436D04) ──
    surf = GetTrackSegmentSurfaceType(&actor.span_raw);   ; +0x80
    if (surf >= 0x10):
        gLateralOffsetBias[i*0x47] = 0

; ── Recovery-stage override (0x436D08..0x436D1C) ──
    rec = actor[i].field_0x37B    ; +0x37B  (recovery stage byte)
    if (rec == 1 || rec == 2):
        gLateralOffsetBias[i*0x47] = 0

; ── Branch-aware lower/upper deviation write (0x436D1C..0x436E70) ──
    ; This is two giant symmetric blocks gated by:
    ;     if (route_state_ptr == DAT_004AFB58)         ; ON MAIN route
    ;        → write LEFT_DEVIATION first  (offset +0x38),
    ;          RIGHT_DEVIATION second
    ;     else                                          ; ON BRANCH route
    ;        → write RIGHT_DEVIATION first, LEFT_DEVIATION second
    ; Each block has slot >= 6 + slot==9-with-encounter exemption that bypasses
    ; ComputeSignedTrackOffset and just copies gLateralOffsetBias[i] to the
    ; corresponding *_DEVIATION slot.
    ; The two final fields gActiveUpperBound[i] = gLeftBounds[i] (or right)
    ; and gActiveLowerBound[i] = gRightBounds[i] (or left) likewise swap.

    INC EBP ; INC LOCAL_8 ; CMP EBP,g_racerCount ; JL → loop top
```

### Region C — Non-drag racer dispatch (0x00436E87..0x00436FAF)

```
test  g_dragRaceMode = [0x4AAF48]
if (g_dragRaceMode != 0):  JMP 0x00437024 (Region C')

EDI = 0          ; slot iter
EBX = &gRaceSlotStateTable                          ; 0x4AADF4 (byte, stride 4)
[ESP+0x10] = &gWantedDamageStateTable               ; 0x4BEAD4 (word, stride 2)
ESI = &actor[0].field_0x33E                         ; encounter_steering_cmd (stride 0x388)
EBP = &gActorForwardTrackComponent                  ; 0x4AFBC0 (int32, stride 0x11C)

LAB_00436ead:
    EAX = min(g_racerCount, 6)                       ; cap at 6
    if (i >= EAX): break

    state = *(byte*)EBX
    if (state == 0):                                 ; AI
        ; Wanted-mode gate: only run AI when (g_wantedMode==0) OR (damage[i]!=0)
        if (g_wantedMode != 0):                     ; [0x004aaf68]
            if (*(word*)&damage[i] == 0):
                goto skip_ai
        UpdateActorTrackBehavior(i)
    else if (state == 2):                            ; FINISHED
        if (*(int*)EBP < 0):                         ; gActorForwardTrackComponent[i] < 0
            *(word*)ESI = 0                          ; encounter_steering_cmd = 0
        else:
            *(byte*)(ESI+0x2F) = 1                   ; brake_flag
            *(word*)ESI       = 0xFF00               ; full reverse-throttle (sign=-1, lo=0)
        *(byte*)(ESI+0x36) = 0xff                    ; …
        *(byte*)(ESI+0x35) = 0xff
        *(byte*)(ESI+0x34) = 0xff
        *(byte*)(ESI+0x33) = 0xff
        *(byte*)(ESI+0x38) = 0
    skip_ai:

    if (state != 3):                                  ; state != INACTIVE
        UpdateVehicleActor(i)

    i++ ; ESI+=0x388 ; EBP+=0x11C ; EBX+=4 ;
    JMP LAB_00436ead

; FALL-THROUGH: traffic dispatch
JMP 0x00436FAA
```

### Region C' — Drag-mode racer dispatch (0x00437024..0x00437079)

Same as Region C but:
- iter `EBX = 0` (not via [ESP+0x10])
- no UpdateActorTrackBehavior call
- no wanted-mode gate
- only the state==2 brake path + state!=3 UpdateVehicleActor

The loop bound differs: `CMP ESI, 0x4B0268` rather than CMP-against-min(racer_count, 6). Effectively iterates while (slot*0x11C < 0x4B0268 - 0x4AFBC0 = 0x6A8 = 6*0x11C+0x14) — i.e., ≤6 slots. Same bound effectively.

### Region D — Non-drag traffic dispatch (0x00436FAF..0x00437023)

```
if (g_dragRaceMode == 0):
    if (g_racerCount <= 6): goto epilogue
    ESI = 6
    while True:
        if (ESI >= min(g_racerCount, 12)): break
        if (ESI == 9 && g_specialEncounterTracked != -1):    ; [0x4B05D8]
            UpdateActorTrackBehavior(9)
            UpdateVehicleActor(9)
            ESI = 10
            continue
        UpdateTrafficRoutePlan(ESI)
        UpdateTrafficActorMotion(ESI)
        ESI++
    if (g_specialEncounterEnabled == 1):                     ; [0x46320C]
        UpdateSpecialTrafficEncounter()
        return
```

### Region D' — Drag-mode traffic dispatch (0x00437024..0x0043708F)

Identical to Region D but inside the `g_dragRaceMode != 0` arm.

### Epilogue (0x00437087-91)

Standard 5-pop frame restore + RET (also at 0x0043701C-23 inside Region D special-encounter early return).

## Key data layouts

| Address | Symbol | Stride | Type | Meaning |
|---|---|---|---|---|
| 0x004AAF00 | g_racerCount | — | i32 | total active racer count |
| 0x004AAF48 | g_dragRaceMode | — | i32 | drag race enabled |
| 0x004AAF68 | g_wantedMode | — | i32 | wanted/cop mode enabled |
| 0x004AADF4 | gRaceSlotStateTable | 4 | u8[2..3 pad] | per-slot state (0=AI, 1=player, 2=finished, 3=inactive) |
| 0x004B05D8 | gSpecialEncounterTracked | — | i32 | tracked-encounter actor handle (-1=none) |
| 0x0046320C | gSpecialEncounterEnabled | — | i32 | master encounter gate |
| 0x004C3D90 | g_trackTotalSpanCount | — | i32 | wraps point for branch detection |
| 0x004C3DA0 | gBranchHeaderPtr | — | ptr | header for branch-vs-main classification |
| 0x004AFB58 | gMainRouteTableBase | — | ptr | LEFT.TRK route bytes (or main route) |
| 0x004B08B4 | gBranchRouteTableBase | — | ptr | RIGHT.TRK route bytes (branch route) |
| 0x004AFB60 | gActorRouteStateTable | 0x11C | i32[] | per-slot route-table pointer |
| 0x004AFB6C | gActorRouteTableSelector | 0x11C | i32 (rs+0x0C) | 0=main, 1=branch |
| 0x004AFB84 | gLateralOffsetBias | 0x11C | i32 (rs+0x24) | per-slot signed lateral bias |
| 0x004AFB98 | gLeftDeviation | 0x11C | i32 (rs+0x38) | left-side route deviation |
| 0x004AFB9C | gRightDeviation | 0x11C | i32 (rs+0x3C) | right-side route deviation |
| 0x004AFBA0 | gLeftBoundsActive | 0x11C | i32 (rs+0x40) | active left extent |
| 0x004AFBA4 | gRightBoundsActive | 0x11C | i32 (rs+0x44) | active right extent |
| 0x004AFBA8 | gLeftBoundsBranch | 0x11C | i32 (rs+0x48) | branch left extent |
| 0x004AFBAC | gRightBoundsBranch | 0x11C | i32 (rs+0x4C) | branch right extent |
| 0x004AFBB0 | gActiveUpperBound | 0x11C | i32 (rs+0x50) | for offset clamp |
| 0x004AFBB4 | gActiveLowerBound | 0x11C | i32 (rs+0x54) | for offset clamp |
| 0x004AFBC0 | gActorForwardTrackComponent | 0x11C | i32 (rs+0x60) | signed forward velocity along route |
| 0x004AFBC4 | gActorTrackSpanProgress | 0x11C | i32 (rs+0x64) | fractional within-span position |
| 0x004AB108 | g_actorRuntimeState | 0x388 | actor[] | per-actor state |
| 0x004AB188 | actor.span_raw | — | i16 | actor+0x80 |
| 0x004AB18A | actor.span_normalized | — | i16 | actor+0x82 |
| 0x004AB2C0 | actor.car_def_ptr | — | ptr | actor+0x1B8 |
| 0x004AB2CC | actor.lin_vel_x | — | i32 | actor+0x1CC |
| 0x004AB2D4 | actor.lin_vel_z | — | i32 | actor+0x1D4 |
| 0x004AB304 | actor.world_pos_x | — | i32 | actor+0x1FC |
| 0x004AB446 | actor.encounter_steer_cmd | — | i16 | actor+0x33E |
| 0x004AB483 | actor.recovery_stage | — | u8 | actor+0x37B |

## Confirmed divergences (port vs original)

### D1 — Branch detection table walker missing **(HIGH IMPACT)**

Port at `td5_ai.c:535-543` collapses the entire branch-classification table walker into a simple "is_span_raw_above_total_span" check:

```c
int span_raw_val = (int)(int16_t)ACTOR_I16(actor, ACTOR_SPAN_RAW);
rs[RS_ROUTE_TABLE_SELECTOR] = (ring_len > 0 && span_raw_val >= ring_len) ? 1 : 0;
```

Original at `0x00436AAD-0x00436B41`:
1. First check matches port: `if (actor.span_raw > g_trackTotalSpanCount)` → branch-route, selector=1.
2. ELSE walks `DAT_004C3DA0` (branch header) and for each entry tests two range conditions on `actor.span_normalized` (NOT span_raw). If conditions match AND the third-condition computation `(branch_offset + ai_span_norm + 1) != 0` is true → assigns `selector=0` + `gActorRouteStateTable=LEFT.TRK base`. If conditions match but third condition fails (==-1), breaks out keeping selector=0.

The port's collapsed test misses:
- (a) the look-up against the branch header,
- (b) the route-table-pointer assignment differing between branch-detected (LEFT.TRK base) vs default (no change to ptr),
- (c) the special "remap_offset == -1" early-exit condition.

**Symptom:** Port wrongly toggles `selector=0` at points where original toggles it to 1 (and vice versa), affecting subsequent forward-track-component / deviation calculations near junctions.

**Note:** This is a `td5_ai_refresh_route_state_slot` divergence. The Phase B audit cannot fix it without the underlying `DAT_004C3DA0` branch-header data, which is loaded by track-loading code that's not yet ported. This pilot will document it but defer the fix until the branch-header data binding is in place.

### D2 — Per-actor track-state work missing in port's `td5_ai_update_race_actors` **(HIGH IMPACT)**

Port's `td5_ai_refresh_route_state_slot` ONLY computes `rs[RS_FORWARD_TRACK_COMP]` (forward velocity projection). The original additionally computes per-actor every tick:

1. `gActorTrackSpanProgress[i]` via `ComputeTrackSpanProgress(span_raw, &world_pos)` — `0x00436BDD`.
2. `RenderTrackSegmentNearActor(rs)` — `0x00436BEF` (loads world transforms for upcoming render).
3. `UpdateActorTrackBounds(i)` — `0x00436BF9`.
4. `ClassifyTrackOffsetClamp(i, current_bias)` + recompute via `ComputeSignedTrackOffset` (clamps bias against active wall extents) — `0x00436C12..0x00436CE8`.
5. `GetTrackSegmentSurfaceType(&span_raw)` ≥ 0x10 → zero the lateral bias — `0x00436CEF..0x00436D04`.
6. Recovery stage 1/2 → zero the lateral bias — `0x00436D0E..0x00436D1A`.
7. Symmetric main-vs-branch deviation write block populating `gLeftDeviation/gRightDeviation/gActiveUpperBound/gActiveLowerBound` — `0x00436D1C..0x00436E70`.

The port does (1)..(7) somewhere else (probably in `td5_track_recompute_actor_offsets`?), but NOT inside the per-slot loop that the original master uses. This means the per-tick TIMING is off relative to physics: the original computes bounds BEFORE the AI/physics dispatch, the port may compute them AFTER (or skip them entirely).

**Verification path:** Capture `gActorTrackSpanProgress[i]` and `gLateralOffsetBias[i]` at the start of each sim_tick from original via Frida; compare to port. If port's progress value lags the original by one tick, that's the missing per-slot work.

**Fix priority:** MEDIUM-HIGH. Likely contributes to AI lateral drift at branch entries. Not addressable until D1 (branch detection) is fixed too.

### D3 — Forward-track-component formula uses sqrt-of-sum-of-squares scale **(LOW IMPACT)**

Port at `td5_ai.c:583-589` (lines):
```c
int32_t vx = ACTOR_I32(actor, ACTOR_LIN_VEL_X) >> 8;
int32_t vz = ACTOR_I32(actor, ACTOR_LIN_VEL_Z) >> 8;
int32_t cos_r = ai_cos_fixed12(forward_heading);
int32_t sin_r = ai_sin_fixed12(forward_heading);
int32_t fwd_comp = (vx * sin_r + vz * cos_r) >> 12;
```

Original at `0x00436B43-0x00436BC4`:
```
EAX = byte * 0x102C      ; (route_byte * 0x102C) signed
CDQ ; AND EDX,0xFF ; ADD EAX,EDX ; SAR EAX,8        ; round-to-zero /256
EBX = sin_v = SinFixed12bit(angle)
EBP = cos_v = CosFixed12bit(angle)
[ESP+0x1C] = sin_v*sin_v + cos_v*cos_v
FILD [ESP+0x1C] ; FSQRT ; __ftol         ; → divisor (≈ 4096)
EDX = actor[i].lin_vel_x = +0x1CC ; SAR EDX,8 → vx>>8
EAX = actor[i].lin_vel_z = +0x1D4 ; SAR EAX,8 → vz>>8
IMUL EAX, EBP   ; (vz>>8) * cos_v
IMUL EDX, EBX   ; (vx>>8) * sin_v
ADD EAX, EDX    ; sum
CDQ ; IDIV ECX  ; signed truncating divide by sqrt(sin²+cos²)
```

Differences:
- (a) `byte * 0x102C` SAR-RZ (round-to-zero) vs port's plain arithmetic SAR by 12. The original computes the SAME `forward_heading` value the port does (since `0x102C * byte` is always a positive product for unsigned route bytes; SAR-RZ degenerates to plain >>8 here). LIKELY equivalent — but only if the route_byte is unsigned.
- (b) Original divisor = `(int)lrintf(sqrt(sin² + cos²))`; port divisor = `12` (i.e., `>> 12`). The sqrt computes the magnitude of the (sin, cos) vector which is approximately 4096 (= `1 << 12`) but varies by LUT quantization. Port loses the per-byte LUT-magnitude-correction. After RC=01 FPU mode fix, sqrt result rounds toward -inf so divisor is slightly smaller than `1 << 12`, producing a fwd_comp slightly LARGER than the port computes.
- (c) Axis pairing: original `vx*sin + vz*cos`, port `vx*sin + vz*cos` — MATCH after re-reading the listing.

**Corrected analysis:** D3 is a magnitude divergence (sqrt-vs-shift) of ≤ ±1 LSB per LUT entry, but NOT an axis flip. The earlier audit claim of axis flip was wrong.

**Verification path:** Capture `gActorForwardTrackComponent[i]` per tick.

### D4 — State-2 (finished) brake fields not written in port **(MEDIUM IMPACT)**

Port at `td5_ai.c:3131-3143` (case 0x02) writes only `ACTOR_BRAKE_FLAG` and `ACTOR_ENCOUNTER_STEER`. Original at `0x00436ED5-0x00436EF7` also clears 5 additional bytes at `ESI+0x33..0x38`:

```
MOV byte ptr [ESI + 0x2f], 0x1     ; +0x36D brake_flag       ; PORT HAS
MOV word ptr [ESI],         0xff00 ; +0x33E encounter_steer  ; PORT HAS  (note: word not just byte)
MOV byte ptr [ESI + 0x36], 0xff    ; +0x374
MOV byte ptr [ESI + 0x35], 0xff    ; +0x373
MOV byte ptr [ESI + 0x34], 0xff    ; +0x372
MOV byte ptr [ESI + 0x33], 0xff    ; +0x371
MOV byte ptr [ESI + 0x38], 0x00    ; +0x376
```

ESI base = `&actor.field_0x33E`, so:
- `+0x2F = actor+0x36D` brake_flag (port writes this).
- `+0x00 = actor+0x33E` encounter_steer (port writes this, but as int16 = `(int16_t)0xFF00 = -256` rather than word `0xFF00 = 65280`; port also conditionally writes only the int16 — sign of 0xFF00 matters).
- `+0x33..0x38 = actor+0x371..0x376`: 5 bytes, currently UNWRITTEN by port.

These are likely a "gear cluster" or "input ramp" zero-out (set to all 0xFF as a sentinel + 0x00 for one).

**Fix:** Add the 5 missing byte writes inside `case 0x02` in `ai_update_single_racer`.

### D5 — State-2 (finished) reverse-vs-brake mode not implemented in port **(MEDIUM IMPACT)**

Port at `td5_ai.c:3131-3143` correctly checks `g_actor_forward_track_component[slot] < 0` but the action sequence differs:

Original:
- `fwd_comp >= 0`: brake_flag=1, encounter_steer=0xFF00 (negative full-throttle marker).
- `fwd_comp < 0`: encounter_steer=0x0000 (zero — no command).

Port:
- `fwd_comp >= 0`: brake_flag=1, encounter_steer=-0x100.
- `fwd_comp < 0`: encounter_steer=0.

The encounter_steer magnitude differs: original=0xFF00, port=-0x100=0xFF00 — **these are actually equal!** `(int16_t)-0x100 = 0xFF00`. So this is bit-equivalent.

**No fix needed for D5.** Confirmed equivalent after closer look.

### D6 — Traffic loop slot-9 hijack inside the loop body **(LOW IMPACT)**

Port at `td5_ai.c:3151-3158` correctly hijacks slot 9 → racer-AI path. Logic byte-equivalent.

### D7 — `local_8` (i.e., the outer "slot" iterator) is an int32 in original but the port keeps a different control flow **(LOW IMPACT)**

Original initializes `[ESP+0x10] = local_8 = 0` and reloads from stack at loop-back via `MOV EBX, [ESP+0x10]` etc. Port uses a C `for (i=0; …)` loop. Effective same iteration, but at the very entry the original uses `(local_8 < g_racerCount)` — for `g_racerCount==0` it skips the body entirely. Port: `for (i=0; i<g_active_actor_count; ++slot)` — same effect.

### D8 — Drag-mode dispatch uses `i < g_racerCount` instead of address-bound **(VERIFY)**

Original drag-mode racer loop (Region C') uses `CMP ESI, 0x4B0268` where `ESI = &gActorForwardTrackComponent[i]`. This computes `i = (ESI - 0x4AFBC0) / 0x11C` = 6 max. The non-drag path uses `min(g_racerCount, 6)`. Equivalent for normal races but for very-low racer-count races (e.g., race with 2 cars), drag-mode iterates ALL 6 actor slots blindly, while non-drag uses `g_racerCount`. This is a subtle behavioural difference — drag mode appears to assume 6 fixed slots regardless of count.

**Port** at `td5_ai.c:3082-3144` (`ai_update_single_racer`) is called from `td5_ai_update_race_actors` which loops `i < min(g_active_actor_count, 6)`. In drag mode, this won't iterate all 6 if `g_active_actor_count < 6`. The original iterates all 6 in drag mode regardless. Likely a bug-vs-bug match — drag mode in TD5 always sets g_racerCount = 6, so the difference is moot, but the port's gate is technically more strict.

**Fix:** Document; no action needed unless drag determines `g_racerCount < 6`.

### D9 — Region D (traffic dispatch) is OUT of `td5_ai_update_race_actors` **(LOW IMPACT)**

Port calls `td5_ai_update_special_encounter()` once after the per-slot loop, but original `0x004369D0` calls `UpdateTrafficRoutePlan + UpdateTrafficActorMotion` per traffic slot 6..11 AND the special encounter check, all inside `UpdateRaceActors`. Port does similar (`ai_update_single_traffic`) but the **physics-side** dispatch (`UpdateVehicleActor`) is NOT being called for traffic from `td5_ai_update_race_actors` — it's deferred to the physics tick.

Original `0x00436F2B-2D` calls `UpdateVehicleActor(i)` for racer slots within `UpdateRaceActors`; the port defers physics dispatch entirely to `td5_physics_tick`. This is a known structural divergence documented in `td5_game.c:2280` ("AI must run BEFORE physics: original UpdateRaceActors @ 0x00436A70 interleaves...").

**Fix priority:** None (already documented).

## Risk class

LOW per scope (dispatcher only). However, **D2 (per-actor track-state work)** is HIGH-IMPACT because the original computes `gActorTrackSpanProgress`, `gLateralOffsetBias`, and the deviation/bounds fields BEFORE per-actor AI dispatch each tick, whereas the port relies on track-tick to compute these AFTER physics. This is a TIMING divergence: bounds reads in `UpdateActorTrackBehavior` will read PRIOR-tick bounds rather than CURRENT-tick bounds.

D1 (branch detection) is HIGH-IMPACT but blocked on missing branch-header data binding.

D3 (forward_track_component formula) is LOW-IMPACT for most tracks (axis flip cancels at 0°/180° headings) but produces wrong sign at 45° headings.

D4 (state-2 cluster writes) is MEDIUM-IMPACT for finished racers in non-time-trial modes.

## Capture schema for pilot

Per call (1 row per slot per tick — one call per `UpdateRaceActors` invocation, slots 0..min(g_racerCount,6) emitted):

**Keys:** `frame`, `sim_tick`, `slot`, `phase` ("entry" | "exit")

**Inputs (call-wide):**
- `network_active` — `g_networkRaceActive` (validated by 0x00432D60 pilot)
- `racer_count`    — `g_racerCount` [0x004AAF00]
- `drag_mode`      — `g_dragRaceMode` [0x004AAF48]
- `wanted_mode`    — `g_wantedMode` [0x004AAF68]
- `encounter_handle` — `gSpecialEncounterTracked` [0x004B05D8]
- `encounter_enabled` — `gSpecialEncounterEnabled` [0x0046320C]
- `track_total_span_count` — `g_trackTotalSpanCount` [0x004C3D90]

**Inputs (per-slot):**
- `slot_state` — `gRaceSlotStateTable[slot]` (byte)
- `span_raw` — `actor[slot].+0x80` (int16)
- `span_norm` — `actor[slot].+0x82` (int16)
- `lin_vel_x` — `actor[slot].+0x1CC` (int32)
- `lin_vel_z` — `actor[slot].+0x1D4` (int32)
- `world_pos_x` — `actor[slot].+0x1FC` (int32)
- `world_pos_z` — `actor[slot].+0x204` (int32)
- `recovery_stage` — `actor[slot].+0x37B` (byte)
- `wanted_damage` — `gWantedDamageStateTable[slot]` (word)

**Outputs (per-slot, after function returns):**
- `route_selector` — `gActorRouteTableSelector[slot*0x47]` (int32)
- `route_table_ptr` — `gActorRouteStateTable[slot*0x47]` (int32 ptr, hex)
- `fwd_track_comp` — `gActorForwardTrackComponent[slot*0x47]` (int32)
- `span_progress` — `gActorTrackSpanProgress[slot*0x47]` (int32)
- `lat_offset_bias` — `gLateralOffsetBias[slot*0x47]` (int32)
- `left_deviation` — `gLeftDeviation[slot*0x47]` (int32)
- `right_deviation` — `gRightDeviation[slot*0x47]` (int32)
- `active_upper_bound` — `gActiveUpperBound[slot*0x47]` (int32)
- `active_lower_bound` — `gActiveLowerBound[slot*0x47]` (int32)
- `encounter_steer` — `actor[slot].+0x33E` (int16)
- `brake_flag` — `actor[slot].+0x36D` (byte)
- `actor_field_371_375` — bytes at +0x371..+0x375 (cluster, 5 bytes packed)
- `actor_field_376` — byte at +0x376

Total: ~30 columns; manageable single CSV.

## Fixes applied (this pilot)

Initial pilot scope is **AUDIT + TRACE** only. The structural divergences (D1, D2) require either:
- Branch-header data binding (D1) — defer to a future pilot once track-loading is fully ported.
- Track-state per-slot work restructuring (D2) — invasive change touching `td5_track_recompute_actor_offsets` + the per-tick ordering.

Initial fixes for this pilot:

1. **D4 — Add 5 missing byte writes** in state-2 (finished) brake path of `ai_update_single_racer`. This is a localized, low-risk change with no upstream blockers.
2. **Wire pilot trace hooks** at `td5_ai_update_race_actors` entry + exit. Emit all 6 racer slot rows per call so the diff can spot per-slot divergences.

D3 (formula divergence) is deferred — it would require restructuring the forward_track_component computation to match the original's sqrt-based magnitude divisor and axis-pairing. Per-slot Frida capture will quantify the divergence first.

## What NOT to do

- Do not "fix" the collapsed branch detection by inventing a branch-header walker without the actual DAT_004C3DA0 data binding — it would write garbage to selector/route_table_ptr.
- Do not change the per-tick ordering of `td5_ai_tick → td5_physics_tick → td5_track_tick` without measuring; the port's current order is documented in `td5_game.c:2280-2306`.
- Do not "fix" D5 — the int16(-0x100) and word(0xFF00) values are bit-equal.
- Do not gate the state-2 byte cluster on `drag_mode` — original writes them in BOTH branches (non-drag racer + drag-mode racer), gate is on slot_state==2 only.

## Reference

- Listing: 0x00436A70..0x00437091 (TD5_pool13, 2026-05-14)
- Decompilation: pool13 session, ~270 lines
- Port: `td5mod/src/td5re/td5_ai.c:3030..3145` (pre-fix)
- Pilot tracer to be added at: `td5mod/src/td5re/td5_pilot_trace_00436A70.{c,h}`
- Frida probe: `tools/frida_pool13_00436A70.js`
- Pool tag: `pool13_00436A70`
