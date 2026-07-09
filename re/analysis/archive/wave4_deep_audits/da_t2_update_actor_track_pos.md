% DA-T2 — Deep Audit of UpdateActorTrackPosition (0x004440F0)
% Date: 2026-05-22  •  Pool: TD5_pool0 (read-only, released at exit)
% Predecessor: re/analysis/pilot_004440F0_audit.md (D1-D7). Adds D8-D10.

# Orientation
- Orig: `UpdateActorTrackPosition` @ 0x004440F0..0x0044541C (4908 B, pure leaf).
- Port: `update_position_recursive` @ td5_track.c:2532; wrappers
  `td5_track_update_actor_position` (chassis) and
  `td5_track_update_probe_position` (per-wheel), both call with `single_step=1`.
- Orig is **single-pass per call** — every case path either RETs or jumps
  to LAB_0044501a (writeback + RET). No outer loop.

# Section A — Orig flow stages

## Stage 1 — Strip-record decode (0x004440F0-0x004441F4)
`TD5_StripSpan` @ `g_trackStripRecords + span_idx * 0x18`:
```
+0x00 b   span_type (uVar18)
+0x03 b   lane_count(low4) | h_offset(hi4)
+0x04 us  left_vertex_index
+0x06 us  right_vertex_index
+0x08 s   link_next
+0x0a s   link_prev
+0x0c i32 origin_x  (integer span-units)
+0x14 i32 origin_z
```
World->local: `iVar13 = (pos_x >> 8) - origin_x`, `iVar19 = (pos_z >> 8) - origin_z`.
Plain `SAR EAX,8` at 0x00444164/0x0044416a (no round-toward-zero idiom).

`edge_mask` seed @ 0x00474e28/0x00474e29 (16B paired by type*2):
```
bVar10 = 0xf
if (sub_lane == 0)              bVar10  = first_mask[type] & 0xf
if (sub_lane == lc - 1)         bVar10 &= last_mask[type]
```
`k_quad_vertex_offsets` @ 0x00474e40/0x00474e41 (paired bytes; only
types 2/3/5/6 nonzero, +/-1):
```
iVar14 = char(LUT[type*2+1]) + right_vertex_index + sub_lane
iVar20 = char(LUT[type*2]  ) + left_vertex_index  + sub_lane
psVar2 = &vp[iVar14]      ("near right")
psVar3 = &vp[iVar14+1]    ("far right")
psVar4 = &vp[iVar20+1]    ("far left")
psVar5 = &vp[iVar20]      ("near left")
```

## Stage 2 — 4-edge cross-products (0x004441F7-0x0044432B)
For each bit, gated by `edge_mask & bit`:
```
bit 1 FWD   cross(psVar5 -> psVar2)   R0 -> L0
bit 2 RIGHT cross(psVar2 -> psVar3)   R1 -> R0
bit 4 BACK  cross(psVar3 -> psVar4)   L1 -> R1
bit 8 LEFT  cross(psVar4 -> psVar5)   L0 -> L1
```
Sign rule: `cross > 0` => outside that edge => bit set in bVar8.

## Stage 3 — Switch dispatch (table @ 0x00445420, 12 entries)
```
1  -> 0x0044434c  FWD-only            (handler)
2  -> 0x004445ae  RIGHT-only          (handler)
3  -> 0x004447cc  FWD+RIGHT compound  (handler)
4  -> 0x004449c1  BACK-only           (handler)
5  -> 0x00445411  *** RET no-op ***
6  -> 0x00444c4c  BACK+RIGHT compound (handler)
7  -> 0x00445411  *** RET no-op ***
8  -> 0x00444e10  LEFT-only           (handler)
9  -> 0x00445032  FWD+LEFT compound   (handler)
10 -> 0x00445411  *** RET no-op ***
11 -> 0x00445411  *** RET no-op ***
12 -> 0x00445214  BACK+LEFT compound  (handler)
```
`MOV EDX,[ESP+0x28]; DEC EDX; CMP EDX,0xb; JA 0x00445411` — values 0 and >0xC also go to no-op.

### Case 1 (FWD only) — 3-way decision tree
1. Seed `bVar10 = 0xf`; if `sub_lane <= 1` use `first_mask[type]`.
2. **Secondary cross #1** gated on `bVar10 & 2`: cross(`psVar2[-3] -> psVar2`)
   (prev-lane's left vertex column, FWD orientation).
   - If > 0: **ADVANCE**. Dispatch by span_type:
     - type 10 (SENTINEL_END): span = link_next; sub_lane += (link_next.lc - cur.lc).
     - type 8  (JUNCTION_FWD): if sub_lane < link_next.lc -> span+1; else span = link_next + delta.
     - else: span+1; sub_lane += (cur.h_offset - new.h_offset).
   - Then `t[2]++; t[3] = max(t[2], t[3]); t[0] = new_span; t[6] = sub_lane - 1`. RET.
3. **Secondary cross #2** gated on `bVar10 & 8`: cross(`psVar5[-3] -> psVar5`).
   - If <= 0: **NO SPAN CHANGE**. Just `t[6] = sub_lane - 1`. RET.
   - If  > 0: span -= 1 (or type-9/11 backward link with delta);
              `t[2]--; t[6] = sub_lane + linkdelta - 1`. RET.

### Case 4 (BACK only) — symmetric to case 1
Mirror with `+3` word offsets and `+1` post-step. `t[2]--`.

### Case 2 (RIGHT only) — advance then 2 retests
1. Dispatch new_span (same as case-1 advance), `t[2]++; t[0] = new_span`.
2. **Retest #1** on new span, `bVar10 & 1` (FWD), vertices `e40[new_sub] -> e41[new_sub]`.
   - If > 0: RET with `t[6] = sub_lane - 1`.
3. **Retest #2**, `bVar10 & 4` (BACK), vertices `e41[new_sub+1] -> e40[new_sub+1]`.
   - If > 0: RET with `t[6] = sub_lane + 1`.
4. Else: LAB_0044501a: `t[6] = (uint8)(sub_lane + 1); t[0] = new_span`. RET.

### Case 8 (LEFT only) — symmetric to case 2
Backward dispatch via type-9/11 link; `t[2]--`. Same two retests.

### Cases 3/6/9/12 (compound) — port already mirrors these
Verified byte-faithful in pilot audit (2026-05-15 pass-4 Ghidra). Each
runs a primary advance + secondary cross with REJECT path; port retains
old span/sub on REJECT.

### Cases 5/7/10/11 and bits == 0 — RET no-op (no state change)

## Stage 4 — Writeback offsets
`track_state` = `short[8]` at actor+0x80:
```
+0  t[0]  span_index
+2  t[1]  span_normalized   (NOT touched here)
+4  t[2]  accumulated_count (+/- per step)
+6  t[3]  high_water        (max of t[2])
+8  t[4]  contact_vertex_A  (NOT touched here)
+a  t[5]  contact_vertex_B  (NOT touched here)
+c  byte  sub_lane_index    (low byte of t[6])
```

# Section B — Port mapping per stage

| Stage           | Port behavior                                                   | Faithful? |
|-----------------|------------------------------------------------------------------|-----------|
| Stage 1 decode  | `compute_boundary_bits` @ td5_track.c:2150, same fields/LUTs    | YES |
| World->local    | Keeps `pos<<8` + `vertex<<8`; int64 cross. Sign-equivalent to orig. | YES (sign-eq.) |
| Stage 2 crosses | `edge_cross` returns sign in {-1,0,+1}; bits OR-combined        | YES |
| Stage 3 case 1  | Calls `resolve_neighbor(0x01)` unconditionally; `sub_lane--`. **No secondary crosses.** | **NO -- D2** |
| Stage 3 case 4  | Same omission, symmetric.                                       | **NO -- D3** |
| Stage 3 case 2  | `resolve_neighbor(0x02)` + `*sub_lane += h_offset`. **No post-step retests.** | **NO -- D8** |
| Stage 3 case 8  | `resolve_neighbor(0x08)` + `*sub_lane -= h_offset`. Same omission. | **NO -- D9** |
| Stage 3 cases 3/6/9/12 | Explicit retests at td5_track.c:2593-2845, byte-faithful | YES |
| Stage 3 cases 5/7/10/11 | Falls into port `default:` at td5_track.c:2848 which cascade-resolves (FWD-or-BACK then RIGHT-or-LEFT). Orig: RET. | **NO -- D10** |
| Outer loop      | `for (iter < TRACK_MAX_RECURSION)` -- both wrappers pass `single_step=1` so exits after 1 iter | Inert |
| memcpy rollback | Only on non-convergence; never reached with `single_step=1`     | Inert |
| `s_jump_entries` net | Port-only safety net for out-of-bounds new_span on non-junction types | **NO -- D7 carryover** |
| sub_lane neg clamp at entry (line 2537) | `if (sub_lane < 0) sub_lane = 0;` -- masks junction step | Likely benign |
| sub_lane wraparound | Port stores `(int8_t)`, orig stores `(byte)`. Same mod 256 result; read-side MOVSX matches. | Benign |
| Stage 4 writeback | Port matches t[0]/t[2]/t[3]/byte-12 offsets | YES |

# Section C — Cascade-relevant divergences

## D2 [HIGH] — Case 1 missing secondary crosses
**Port:** td5_track.c:2564-2585. **Orig:** 0x0044434c-0x004445ad.
Orig has 3 outcomes; port always takes outcome #1 (advance). For an AI
car with lateral velocity at lane boundaries, orig often keeps the span
unchanged (outcome #3, "STAY") and just shifts sub_lane. Port advances.
Over the 121-tick countdown that compounds: aligns with the **+35 u/tick
peer-push divergence** in `reference_pool13_dynamic_diff_2026-05-21` and
plausibly explains residue after the sqrt-divisor fix
(`reference_fwd_track_comp_sqrt_divisor_2026-05-21`, slot 0 bias 11u->32u).

## D3 [HIGH] — Case 4 missing secondary crosses
**Port:** td5_track.c:2646-2661. **Orig:** 0x004449c1-0x00444c4b.
Symmetric to D2. Triggers during deceleration/reverse + backward
junctions. Unblocks `[[todo-reverse-not-triggered]]` and the Newcastle
span 216 invisible-wall path.

## D8 [HIGH-NEW] — Case 2 (RIGHT-only) missing post-step retests
**Port:** td5_track.c:2587-2591. **Orig:** 0x004445ae-0x004449c0.
Orig commits forward step, then 2 retests on the new span gate the
final sub_lane offset:
- Retest FWD on new span -> `sub_lane - 1`
- Retest BACK on new span -> `sub_lane + 1`
- Neither fires -> `sub_lane + 1` (fall-through LAB_0044501a)

Port uses only `*sub_lane += h_offset`. Always overshoots when the new
span's geometry crosses the FWD edge. Edinburgh's long sweeping
right-hand bends use this path heavily; high-confidence cascade source.

## D9 [HIGH-NEW] — Case 8 (LEFT-only) missing post-step retests
**Port:** td5_track.c:2725-2729. **Orig:** 0x00444e10-0x00445031.
Symmetric to D8 for the LEFT-diagonal step. Affects Sydney/BlueRidge AI
cars hugging the left wall and feeds into the wheel_contact_delta cascade
(`reference_wheel_contact_delta_field`).

## D10 [MEDIUM-NEW] — Default cascade-resolution for impossible bit patterns
**Port:** td5_track.c:2848-2872. **Orig:** bits 5/7/10/11 -> RET no-op.
These bit patterns indicate degenerate geometry. Orig trusts the next
tick to disambiguate; port warps span by +/- 1 every tick the actor
sits on the degenerate quad. Possible failure modes: lap-complete gate
miss (high-water corruption), AI bias re-trigger.
**Pre-flight required:** Frida-side bits histogram over an Edinburgh
6-racer race to confirm these patterns ever occur. If never seen, the
fix is a no-op cleanup.

## D7 [LOW carryover] — `s_jump_entries` heuristic
Already guarded to fire only when `new_span` is out of bounds AND
span_type is not 8/9/10/11. Empirically fires near Newcastle span 216
junction-adjacent geometry. Defer until junction-table audit lands.

## D1, D5, D6 — PASS / INERT
D1 (SAR semantics) confirmed at disasm level — no fix needed.
D5 (TRACK_MAX_RECURSION loop) inert with `single_step=1`.
D6 (camera-probe stub) doesn't affect cascade.

# Section D — Severity ranking

| Tag | Sev | Estimated lift @ sub_tick=1 | Evidence |
|-----|-----|-----------------------------|----------|
| D2  | HIGH | 10-30 fields | Pool13 +35u/tick peer-push residue; slot 0 bias 11u->32u trade-off |
| D8  | HIGH-NEW | 10-20 fields | Edinburgh right-hand bend frequency (every span entry) |
| D9  | HIGH-NEW | 5-15 fields  | Sydney/BlueRidge LEFT-wall hug + wheel_contact_delta cascade |
| D3  | HIGH | unknown | Unblocks `[[todo-reverse-not-triggered]]` and Newcastle backward junctions |
| D10 | MED-NEW | unknown | No runtime evidence yet — need Frida histogram first |
| D7  | LOW | nil | Already gated; defer to junction-table audit |
| D1  | PASS | nil | Disasm confirmed plain SAR |
| D5/D6 | INERT | nil | Single_step=1 + camera stub bypass |

# Section E — Actionable fixes

## Fix 1 (D2) — Case 1 secondary crosses
At td5_track.c:2564, replace unconditional advance with:
```c
/* Orig 0x0044436d gate: bVar10 & 2 on (psVar2[-3] -> psVar2). */
TD5_StripSpan *sp = &s_span_array[span_idx];
int type = sp->span_type;
int e40 = (int)sp->left_vertex_index  + k_quad_vertex_offsets[type][0];
int e41 = (int)sp->right_vertex_index + k_quad_vertex_offsets[type][1];
uint8_t mask = (sub_lane <= 1) ? s_edge_mask_first[type] : 0x0f;
int64_t cadv = (mask & 0x02)
    ? compound_cross(sp, e41 + sub_lane - 1, e41 + sub_lane, pos_x, pos_z) : 1;
if (cadv > 0) {
    /* ADVANCE (current port behavior + sub_lane--) */
} else {
    int64_t cback = (mask & 0x08)
        ? compound_cross(sp, e40 + sub_lane, e40 + sub_lane - 1, pos_x, pos_z) : 0;
    if (cback <= 0) {  /* STAY: only sub_lane-- */
        sub_lane -= 1;
        ((int8_t *)track_state)[12] = (int8_t)sub_lane;
        return;
    }
    /* STEP BACK: span-1 or type-9/11 link with delta, t[2]--, sub_lane-- */
}
```

## Fix 2 (D3) — Case 4 mirror of Fix 1
Mask = `(sub_lane >= lc - 2) ? s_edge_mask_last[type] : 0x0f`; gate on
`mask & 0x02` with vertices shifted +1; post-step `sub_lane += 1`.

## Fix 3 (D8) — Case 2 post-step retests
After `resolve_neighbor(0x02)`, before `break`:
```c
TD5_StripSpan *nsp = &s_span_array[new_span];
int nt = nsp->span_type, nlc = span_lane_count(nsp);
int new_sub = sub_lane;
int e40 = (int)nsp->left_vertex_index  + k_quad_vertex_offsets[nt][0];
int e41 = (int)nsp->right_vertex_index + k_quad_vertex_offsets[nt][1];
uint8_t mask = 0x0f;
if (new_sub == 0)       mask  = s_edge_mask_first[nt];
if (new_sub >= nlc - 1) mask &= s_edge_mask_last[nt];
if ((mask & 0x01) && compound_cross(nsp, e40+new_sub, e41+new_sub, pos_x, pos_z) > 0) {
    ((int8_t*)track_state)[12] = (int8_t)(new_sub - 1); return;
}
if ((mask & 0x04) && compound_cross(nsp, e41+new_sub+1, e40+new_sub+1, pos_x, pos_z) > 0) {
    ((int8_t*)track_state)[12] = (int8_t)(new_sub + 1); return;
}
((int8_t*)track_state)[12] = (int8_t)(new_sub + 1); return;
```

## Fix 4 (D9) — Case 8 mirror of Fix 3
Backward dispatch + `t[2]--`. Retests anchored to left-edge column.

## Fix 5 (D10) — Replace port default with no-op (RET)
Pre-flight: Frida bits histogram over Edinburgh 6-racer to confirm
these patterns actually fire. If never seen, fix is a cleanup; if they
fire, also closes part of `[[todo-newcastle-span-216-invisible-wall]]`.

## Sequencing
Apply Fix 1+3 together, diff-race, measure pool13 cascade lift. Then
2+4. Fix 5 only after histogram pre-flight. Defer D7 until
junction-table audit lands.

# Wrap-up
- No prior memory note covers D8/D9/D10 specifically.
- D2/D3 carry over from pilot_004440F0_audit.md with concrete pseudocode.
- Pool slot TD5_pool0 released; read-only session — no writes.
- Time-box held (~75 min wall).

Refs: pilot_004440F0_audit.md • reference_pool13_dynamic_diff_2026-05-21 •
reference_fwd_track_comp_sqrt_divisor_2026-05-21 •
td5mod/src/td5re/td5_track.c:2150..2930.
