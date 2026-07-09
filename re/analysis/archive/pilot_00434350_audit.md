# Pilot Audit — 0x00434350 InitializeActorTrackPose

**Date:** 2026-05-14
**Pool slot:** TD5_pool14
**Port-side function:** split across THREE call sites in `td5_game.c` per-slot
spawn loop (around line 1390-1480):
  1. `td5_track_compute_heading(actor)` @ `td5_track.c:3074` — yaw write half.
  2. `td5_physics_reset_actor_state(actor)` @ `td5_physics.c:5671` — reset half
     (mirror of `ResetVehicleActorState` @ 0x00405D70).
  3. `td5_ai_seed_actor_track_progress_offset(slot)` @ `td5_ai.c:1458` — final
     ComputeTrackSpanProgress + ComputeSignedTrackOffset writeback.

**Worktree:** `.claude/worktrees/precise-00434350` on branch `precise-00434350`
**Caller graph:** `InitializeRaceSession` @ 0x0042AA10 only. 17 call sites:
  - SINGLE_RACE non-circuit: 6 ordered calls (slot 0..5) at -9, -6, -3, -12,
    base, -15 offsets, sub_lanes {1,2,1,2,1,2}, **flip=0**.
  - SINGLE_RACE circuit: 6 ordered calls at -12, -6, -12, -6, base-18, base-18
    pairs, sub_lanes {1,2,1,2,1,2}, **flip=0**.
  - WANTED mode: 6 calls at -3, +0x19, +0x32, +0x4B, +0x64, +0x7D offsets,
    sub_lanes {2,2,3,3,2,3}, **flip=0**.
  - DRAG (`g_raceOverlayPresetMode == 1`): hardcoded `(0,115,1,0)` + 5 calls
    `(i, 1, i-1, 0)`, **flip=0**.
  - NETWORK_RACE branch: loop emits `(slot, sVar12, ((uVar6 & 1) + 1) or 0,
    **0**)` per slot.

  **Every single call passes `param_4 = 0`**, so the flip-rotation branch
  (0x00434515 ADD EAX, 0x80000) is **dead code** in this binary build.
**Callee graph:** InitActorTrackSegmentPlacement (0x445F10), AngleFromVector12
  (0x40A720), ResetVehicleActorState (0x405D70), ComputeTrackSpanProgress
  (0x4345B0), ComputeSignedTrackOffset (0x434670).
**Body:** 0x00434350..0x00434587 (0x238 bytes / 195 instructions / 95 decompiled
lines).

## Function structure (from listing)

```
prologue                              ; save EBX/EBP/ESI/EDI
write actor+0x80 = (short)param_2     ; span_raw [@0x00434377]
write actor+0x8c = (char) param_3     ; sub_lane [@0x00434383]
call InitActorTrackSegmentPlacement(&actor->field_0x80, &actor->world_pos)
                                       ; writes actor+0x84 = actor+0x86 = span,
                                       ; clamps actor+0x8c, fills world_pos XYZ

load sp = &g_trackStripRecords[actor+0x80 * 0x18]
psVar2 = &g_trackVertexPool[(u16)sp->left_vertex_index * 6]   ; vl0 base
psVar3 = &g_trackVertexPool[(u16)sp->right_vertex_index * 6]  ; vr0 base
uVar7 = uVar10 = param_1                ; PRE-INIT to slot id (for default br)
switch ((u8)sp->span_type - 1) {
case 0,1,4 (types 1,2,5):
    iVar6 = (vl1.x - vr1.x) - vr0.x + vl0.x           ; dx
    iVar6 = iVar6 + ((iVar6 >> 31) & 3)                ; round-to-zero /4 bias
    iVar5 = vl1.z - vr1.z                              ; dz (partial)
    sVar4 = vr0.z
    GOTO LAB_00434434
case 2,3 (types 3,4):
    iVar6 = (vl1.x - vr2.x) - vr1.x + vl0.x           ; uses vr2 (right+2)
    iVar6 = iVar6 + ((iVar6 >> 31) & 3)
    iVar5 = vl1.z - vr2.z
    sVar4 = vr1.z
LAB_00434434:
    iVar5 = (iVar5 - sVar4) + vl0.z                    ; dz
    uVar7 = (iVar5 + ((iVar5 >> 31) & 3)) >> 2         ; dz/4 RZ
    uVar10 = iVar6 >> 2                                ; dx/4
    break;
case 5,6 (types 6,7):
    iVar6 = (vl2.x - vr1.x) + vl1.x - vr0.x            ; uses vl2 (left+2)
    iVar5 = (vl2.z - vr1.z) - vr0.z + vl1.z            ; dz
    uVar7 = (iVar5 + ((iVar5 >> 31) & 3)) >> 2         ; dz/4 RZ
    uVar10 = (iVar6 + ((iVar6 >> 31) & 3)) >> 2        ; dx/4 RZ
    break;
default:                               ; LAB_0043448c
    EBX = uVar10 = [ESP+0x14] = param_1 (slot)
    ECX = uVar7  = [ESP+0x14] = param_1 (slot)
                                       ; falls through to 4-quadrant dispatch
}
LAB_00434494: 4-quadrant dispatch:
    if (uVar10 < 0) GOTO LAB_004344c8  ; dx < 0
    if (uVar7  < 0) GOTO LAB_004344a8  ; dx >= 0, dz < 0
    // dx >= 0, dz >= 0
    angle = AngleFromVector12(uVar10, uVar7);          ; +0
    GOTO LAB_00434501
LAB_004344a8: dx > 0, dz < 0
    if (dx == 0 || dz >= 0) GOTO LAB_004344eb
    angle = AngleFromVector12(abs(uVar7), uVar10) + 0x400
    GOTO LAB_00434501
LAB_004344c8: dx < 0, OR dx == 0 && dz < 0
    if (dz >= 0) GOTO LAB_004344eb
    angle = AngleFromVector12(abs(uVar10), abs(uVar7)) + 0x800
    GOTO LAB_00434501
LAB_004344eb: dx < 0, dz >= 0
    angle = AngleFromVector12(uVar7, abs(uVar10)) + 0xc00
LAB_00434501:
    angle = (angle + 0x800) << 8
    write actor+0x1F4 = angle           ; yaw_accum [@0x0043450F]
    if (param_4 != 0) {
        angle += 0x80000
        write actor+0x1F4 = angle       ; +180° rotation [DEAD: all calls flip=0]
    }

call ResetVehicleActorState(actor)      ; zeros velocities, sets gear=2,
                                        ; engine=400, world_pos_y=-0x40000000,
                                        ; then IntegrateVehiclePoseAndContacts.
                                        ; Reads euler_accum_yaw (just-written)
                                        ; to seed display_angle_yaw = yaw>>8.

progress = ComputeTrackSpanProgress(actor+0x80 /raw/, &actor->world_pos)
gActorTrackSpanProgress[param_1 * 0x47] = (int)progress

route_bytes = (byte*)gActorRouteStateTable[param_1 * 0x47]
sVar4 = actor[+0x82]                    ; SPAN_NORMALIZED — uninitialised at
                                        ; this point (not written by
                                        ; InitActorTrackSegmentPlacement)
route_byte = route_bytes[sVar4 * 3]     ; reads route_bytes[0] when 0x82 == 0
bias = ComputeSignedTrackOffset(actor+0x80, progress, route_byte)
DAT_004afb84[param_1 * 0x47] = bias
return
```

## Key arithmetic primitives

`sar_rz4(x)` — round-to-zero signed divide by 4 (instances at 0x004343EC,
0x0043441C, 0x0043443D, 0x0043445F, 0x0043447D):
```
CDQ
AND EDX, 0x3
ADD EAX, EDX
SAR EAX, 2
```
Equivalent C: `(x + ((x >> 31) & 3)) >> 2`. For positive x equals `x >> 2`;
for negative x not divisible by 4 it rounds one unit closer to zero than plain
SAR. Port uses this idiom in `td5_track_compute_heading` at lines 3133-3134,
3144-3145, 3155-3156. **Byte-faithful.**

`abs(x)` via CDQ/XOR/SUB idiom (instances at 0x004344B2-B5, 0x004344CE-D1,
0x004344D7-D9, 0x004344ED-F0):
```
MOV EAX, x
CDQ
XOR EAX, EDX
SUB EAX, EDX
```
Plain `abs()`. Port uses `-dx` / `-dz` directly in `angle_from_vector_full`
(unused) and via `AngleFromVector12` octant dispatch in `td5_render.c`. The
literal port at `td5_render.c:4525` is byte-faithful with the original LUT.

`AngleFromVector12` (0x0040A720) — 4-quadrant 12-bit angle helper. Port: literal
LUT-based port already in place at `td5_render.c:4525-4720`. This pilot does
NOT modify it.

## Confirmed divergences from port (pre-fix at session start)

### D1 — Default branch fallthrough sets dx=dz=slot, port hardcodes dx=0,dz=1 **(LOW IMPACT)**

Original disassembly at 0x0043448C-94:
```
0043448C MOV EBX, [ESP+0x14]    ; = param_1 = slot
00434490 MOV ECX, [ESP+0x14]    ; = param_1 = slot
```
So when `span_type ∈ {0, 8, 9, 10, 11}` or any non-1-7 value, both dx (EBX) and
dz (ECX) take the slot id. Then `AngleFromVector12(slot, slot)`.

Port at `td5_track.c:3160`:
```c
default:
    dx = 0; dz = 1;
    break;
```

Default angle (port): `AngleFromVector12(0, 1) = 0` (octant 0, p1=0). Plus +0x800
bias → `(0 + 0x800) << 8 = 0x80000` = 180° accum.

Default angle (orig): `AngleFromVector12(slot, slot)`:
- slot=0: returns 0 (entry guard `param_1==0 && param_2==0`).
- slot>=1: octant 1 path → idx=(N\*1024 + N/2)/N = 1024 → LUT[1024]=512.
  Returns 0x400 - 512 = -112 (signed). Bias → ((-112 + 0x800) \* 0x100) =
  0x77000.

Stored yaw_accum (orig, slot 0): 0x80000 (same as port).
Stored yaw_accum (orig, slot 1): 0x77000 (port writes 0x80000).
Stored yaw_accum (orig, slots 2-5): all 0x77000 (port writes 0x80000).

**Impact:** for slot != 0 on a span_type outside [1,7], port writes a 30°
yaw_accum mismatch. In practice the Edinburgh + Moscow + drag spawn spans are
type 1-7 so this never fires. But for any track whose start_span lands on a
type 0/8/9/10/11 span (junction spans, e.g. some Moscow start spans on
RIGHT.TRK route are reported type 9/10 in `todo_ai_right_route_branches.md`),
the divergence shows.

**Fix:** replace port default with `dx = (int32_t)slot; dz = (int32_t)slot;`
so AngleFromVector12 then produces the original's quasi-180°-ish result.

### D2 — Sub_lane writeback omission **(LOW IMPACT)**

Original `InitActorTrackSegmentPlacement` clamps `actor+0x8c` to
`(lane_count_nibble) - 1` and writes the clamped value back into the actor:
```
if ((int)(pbVar1[3] & 0xf) <= iVar7) {
    iVar7 = (pbVar1[3] & 0xf) - 1;
    *(char *)(param_1 + 6) = (char)iVar7;          ; writes actor+0x8c
}
```

Port at `td5_game.c:1394`:
```c
actor[0x08C] = (uint8_t)sub_lane;                  ; writes RAW value
```
followed by `td5_track_get_span_lane_world(span_index, sub_lane, ...)` which
internally clamps but does NOT write the clamped value back to the actor.

**Impact:** if the spawn-table sub_lane (1, 2, or 3) exceeds the span's
`lane_count`, the port's actor field holds an over-large lane number while the
original writes back the clamped value. Subsequent reads of actor+0x8c
(e.g. `td5_ai_seed_actor_track_progress_offset` at line 1463 reads it as
`sub_lane`-indexed route byte? — no, that's `span_norm * 3 + route_table`,
no sub_lane involvement) may then index into wrong data.

The most exposed reader is `td5_track_compute_heading` itself
(`td5_track.c:3100`):
```c
sub_lane = (int)((uint8_t *)actor)[0x8C];
```
…but that value is **NOT consumed** by the case 1/2/5/3/4/6/7 code paths —
they all derive dx/dz from `sp->left_vertex_index` and `sp->right_vertex_index`
without lane offset. (Compare port's `get_quad_vertices` which DOES add
`sub_lane` to compute the lane-corrected world position used by
`td5_track_get_span_lane_world`.)

**Fix:** add a clamp+writeback step in `td5_game.c` spawn loop BEFORE writing
actor+0x8c, using `span_lane_count(sp)` from `td5_track`. The clamped value
becomes the new sub_lane for both actor write and the subsequent
`get_span_lane_world` call.

### D3 — Yaw write order: port writes yaw BEFORE reset; original writes yaw THEN reset **(NEGLIGIBLE — confirmed equivalent)**

Both port and original write `actor+0x1F4` before calling reset, so display_angle_yaw seeded in reset_actor_state (line 5694 port / `actor->display_angle_yaw = (short)((uint)actor->euler_accum_yaw >> 8)`) reads the just-written value in both cases. **No divergence.**

### D4 — actor+0x82 (span_normalized) pre-init in td5_game.c **(MEDIUM IMPACT — currently masked at AI seed)**

Original sequence:
1. `MOV [EAX], CX`         writes actor+0x80 (span_raw)
2. `MOV [ESI+0x4ab194], DL` writes actor+0x8c (sub_lane)
3. `CALL InitActorTrackSegmentPlacement` writes actor+0x84, +0x86 (NOT +0x82)
4. ... yaw computation + ResetVehicleActorState (which calls
   IntegrateVehiclePoseAndContacts which calls NormalizeActorTrackWrapState
   which finally writes actor+0x82)
5. `MOVSX EDX, word ptr [ESI + 0x4ab188]`  re-reads actor+0x80 (NOT +0x82)
   for ComputeTrackSpanProgress arg
6. `MOVSX ECX, word ptr [ESI + 0x4ab18a]`  reads **actor+0x82** for the route
   index. **At this point actor+0x82 has been written by the integrate-pose
   path inside ResetVehicleActorState.** So sVar4 = the just-normalized span.

Wait — re-reading. `ResetVehicleActorState` calls
`IntegrateVehiclePoseAndContacts` which calls
`UpdateActorTrackPositionFromContacts` → `NormalizeActorTrackWrapState`. The
normalize writes actor+0x82 to wrap(actor+0x84 / span_ring_length). After the
reset returns, actor+0x82 = normalized(span_raw).

For slot 0 on Edinburgh, span_raw = 62 (or 53 depending on spawn offset). After
normalize, span_norm = 62 mod ~580 = 62. So sVar4 = 62 at the route-byte read.

Port at `td5_game.c:1391` pre-emptively writes `actor+0x82 = span_index`.
Then port's `td5_ai_seed_actor_track_progress_offset` reads `actor+0x82` —
which already equals span_index. The values match.

**However**, the port's seed function comment at td5_ai.c:1480-1498 claims
"the original reads actor+0x82 BEFORE it has been populated" and "reads
route_table[0]" instead. The Frida datum cited says "all 6 slots get
route_byte=106 during init" — this matches `route_table[0]` IF AND ONLY IF
`actor+0x82 == 0` when the route_byte is read.

Re-reading the original listing more carefully:
- The actor memory is zero-initialised before `InitializeRaceSession`
  (`InitializeRaceVehicleRuntime` performs the memset).
- `InitActorTrackSegmentPlacement` only writes +0x84 and +0x86 (not +0x82).
- `ResetVehicleActorState` calls `IntegrateVehiclePoseAndContacts` which
  contains the chain that eventually writes +0x82.

So whether `actor+0x82 == span_norm` or `actor+0x82 == 0` at the moment the
route_byte index is computed depends on whether the integrate-pose chain has
already written +0x82 BEFORE the route-byte read.

This is observable: the Frida trace at fix-1778389787-4116 reports all 6
init-phase ComputeSignedTrackOffset calls receive route_byte = 106 = the
zero-th route byte. That datum is **only consistent with actor+0x82 == 0**
at the route-index read, meaning IntegrateVehiclePoseAndContacts does NOT
write +0x82 along this path (perhaps because actor->surface_contact_flags == 0
gates the normalize call, or because the just-written world_pos_y of
0xC0000000 fails a sanity check upstream of normalize).

**Conclusion:** the port's seed function (`td5_ai.c:1499-1503`) deliberately
hard-codes `route_bytes[0]` to match original behaviour, AND `td5_game.c:1391`
pre-writes `actor+0x82 = spawn_span` for a different reason (the comment at
line 1385-1389 cites P2P checkpoint behaviour). These two writes interact —
the +0x82 pre-init makes `actor+0x82` non-zero at AI-seed-time, but the AI
seed function reads `route_bytes[0]` anyway.

The semantic question is: **does the original actor+0x82 stay 0 at the seed
point, and does the port's pre-write change downstream behaviour?**

Searching the codebase, `actor+0x82` is consumed primarily by:
- `td5_ai_seed_actor_track_progress_offset` (the function in question)
- `td5_ai_get_route_state_ring` for normalized-span lookups
- Wall response / track update functions

The most exposed downstream effect is `td5_ai_get_route_state_ring(slot)` if
called before the first physics tick reaches normalize. Per the Frida bias
mismatch (port +443 vs orig -297 in 2026-05-11) it appears the port
historically over-wrote +0x82, then the seed read it back, producing a
wrong-side bias. The current port workaround in
`td5_ai_seed_actor_track_progress_offset` papers over this with the explicit
`route_bytes[0]` read.

**Fix (cleaner):** stop pre-writing actor+0x82 in `td5_game.c:1391`. Leave
it as the zero-init memset, matching original. Then revert the
`route_bytes[0]` workaround in seed and use `route_bytes[span_norm * 3]`,
which on a freshly-init'd actor reads `route_bytes[0]`.

  **HOWEVER**: per the comment at `td5_game.c:1385-1389`, removing the +0x82
  write was tried and produced P2P checkpoint-threshold misfires. This is
  cross-coupled with the wrap-state normalize flow and is out of scope for
  the 0x00434350 pilot. **Leave as-is and document.**

### D5 — Port writes actor+0x375 (slot id) and actor+0x371..+0x374 (emitter sentinels) NOT done by original **(LOW IMPACT — pure port additions)**

Lines 1417-1421 of `td5_game.c`:
```c
actor[0x375] = (uint8_t)slot;
memset(actor + 0x371, 0xFF, 4);                   ; wheel emitter ids
```
Original `InitializeActorTrackPose` writes neither.

The slot-id write at +0x375 is plausibly written elsewhere in the original's
spawn chain (e.g. `InitializeRaceVehicleRuntime` which the port maps to
`td5_game.c` directly). The +0x371..+0x374 emitter sentinels are explicitly a
port enhancement to satisfy the VFX module's "0xFF == free slot" sentinel.

Frida confirms these fields are at 0xFF in the original at sim_tick=1, so the
port's pre-init is faithful to OBSERVABLE state — just not to the
0x00434350 callee. No fix needed.

### D6 — Port `td5_physics_reset_actor_state` zeros wheel_load_accum to 0 after IntegratePose; original does NOT **(see precise-00405D70 pilot)**

`td5_physics.c:5776-5781`:
```c
for (int i = 0; i < 4; i++)
    actor->wheel_load_accum[i] = 0;
actor->angular_velocity_roll = 0;
actor->angular_velocity_pitch = 0;
actor->linear_velocity_y = 0;
```
Original `ResetVehicleActorState` zeros wheel_suspension_vel (NOT
wheel_load_accum), angular_velocity_roll/pitch, linear_velocity_y. The port's
wheel_load_accum=0 sweep is a port-side approximation that the
precise-00405D70 pilot may revisit. Out of scope for 0x00434350.

### D7 — AngleFromVector12 byte-faithfulness via td5_render.c **(VERIFIED OK)**

The port's call `AngleFromVector12(dx, dz) & 0xFFF` in `td5_track_compute_heading`
resolves to the literal LUT-based port at `td5_render.c:4525`. The port writes
the same `(angle + 0x800) << 8` to actor+0x1F4 as the original does after the
4-quadrant dispatch. **Verified equivalent for typical inputs.**

  **Watch-out:** the port's `td5_track_compute_heading` calls `AngleFromVector12`
  ONCE with the raw `(dx, dz)`. The original calls it FOUR ways with different
  quadrant-decomposed arg orders. Because the literal port at td5_render.c
  internally implements all 8 octants, the single call IS equivalent to the
  original's 4-quadrant dispatch IF AND ONLY IF the literal port matches the
  4-quadrant-dispatched original byte-for-byte. The pilot for 0x0040A720
  validated this. Re-confirmed.

## Static fix priority (apply in worktree, this pilot)

1. **D1** (default branch dx=dz=slot) — small, surfaces on degenerate span
   types. Replace `dx=0; dz=1;` with `dx=dz=slot` (passed in via a new param
   on `td5_track_compute_heading` or read from `actor` via slot inference).

2. **D2** (sub_lane writeback) — small, makes spawn invariant under invalid
   lane numbers. Add `int clamped = clamp(sub_lane, 0, lane_count-1);` then
   `actor[0x8C] = clamped;`.

Skip D4/D6/D7 — out of scope for this pilot; covered by other precise-port
slots (precise-00405D70, precise-00404030, precise-00405E80) or already
verified.

## Capture schema for pilot (Frida + port)

Per init call (1 row per slot per race):
**Keys:** `slot`, `caller_label`
**Inputs:**
- `param_2_span`               — short, passed by InitializeRaceSession
- `param_3_sub_lane`           — char (signed!)
- `param_4_flip_flag`          — int (always 0 in observed binary)
- `span_type`                  — read from sp->span_type
- `left_vi`, `right_vi`        — sp->left_vertex_index/right_vertex_index
- `vl0_x`, `vl0_z`, `vl1_x`, `vl1_z`, `vr0_x`, `vr0_z`, `vr1_x`, `vr1_z`
  — and `vl2_x/z`/`vr2_x/z` for cases 3/4/6/7
- `lane_count_nibble`          — (sp->byte_3 & 0x0F)

**Outputs:**
- `dx_raw`, `dz_raw`           — pre-SAR/4 deltas (case-specific)
- `dx_div4`, `dz_div4`         — after RZ /4
- `quadrant`                   — 0/0x400/0x800/0xC00 selector
- `angle_from_vec12`           — 0..0xFFF
- `actor_yaw_accum`            — final write at actor+0x1F4
- `actor_sub_lane_post`        — actor+0x8C post-InitActorTrackSegmentPlacement
                                  (tests D2 clamp writeback)
- `actor_span_norm_at_seed`    — actor+0x82 value at the moment of route-byte
                                  read (tests D4)
- `progress`, `route_byte`, `bias` — outputs of ComputeTrackSpanProgress and
                                     ComputeSignedTrackOffset.

Captures 6 slots × ~1 race = 6 rows per race. Init-only window — capture must
include sim_tick 0 (use `--RaceTraceMaxSimTicks=10` upstream and key on
caller_label="init" to filter).

## Fixes to apply (this pilot worktree)

### Fix D1 — default span_type writes dx=dz=slot

`td5_track.c:3074` `td5_track_compute_heading`:
- Add `int slot` parameter, or thread the slot id via actor pointer arithmetic
  (actor offset from `s_actor_memory` / TD5_ACTOR_STRIDE).
- Replace `default: dx = 0; dz = 1;` with `default: dx = slot; dz = slot;`.
- Audit all 3 callers in td5_game.c (line 1427) and td5_ai.c (lines 2327, 2473)
  to pass slot.

Slot can also be inferred from `((uint8_t*)actor - s_actor_memory) / TD5_ACTOR_STRIDE`
inside compute_heading itself — keeps the signature unchanged. Pick this route
for the pilot.

### Fix D2 — sub_lane clamp writeback in spawn loop

`td5_game.c:1394` change:
```c
actor[0x08C] = (uint8_t)sub_lane;
```
to:
```c
{
    int lane_count = td5_track_span_lane_count_at(span_index);
    int clamped_sub_lane = sub_lane;
    if (clamped_sub_lane >= lane_count && lane_count > 0)
        clamped_sub_lane = lane_count - 1;
    if (clamped_sub_lane < 0)
        clamped_sub_lane = 0;
    actor[0x08C] = (uint8_t)clamped_sub_lane;
    sub_lane = clamped_sub_lane;  /* feed back into world_pos query */
}
```
(`td5_track_span_lane_count_at` needs adding to `td5_track.h` since
`span_lane_count(sp)` is `static` in td5_track.c.)

## What NOT to do

- **Do not** add the param_4 flip-rotation branch. All 17 observed call sites
  pass 0; the +0x80000 branch is dead code in the binary.
- **Do not** rewrite the +0x82 pre-write or AI seed `route_bytes[0]`
  workaround. They are cross-coupled with the P2P / normalize chain and
  belong to precise-00405D70 / precise-00443FB0 pilots.
- **Do not** touch ResetVehicleActorState (precise-00405D70 pilot).
- **Do not** try to merge `td5_track_compute_heading` and the surrounding
  reset / seed calls into one function. The split has accumulated logging,
  comments, and test surface that's easier to audit per-half.

## Reference

- Listing: 0x00434350..0x00434587 (Ghidra TD5_pool14, 2026-05-14)
- Decompilation: same session, 95 lines
- Port (yaw half): `td5mod/src/td5re/td5_track.c:3074-3184`
- Port (reset half): `td5mod/src/td5re/td5_physics.c:5671-5783`
- Port (seed half): `td5mod/src/td5re/td5_ai.c:1458-1529`
- Port (caller): `td5mod/src/td5re/td5_game.c:1311-1491`
- Frida probe (this pilot): `tools/frida_pool14_00434350.js`
- Port trace emitter: `td5mod/src/td5re/td5_pilot_trace_00434350.{c,h}`
- Tag: `pool14_00434350`
