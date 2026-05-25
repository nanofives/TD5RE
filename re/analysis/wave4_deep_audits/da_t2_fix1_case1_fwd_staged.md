# DA-T2 Fix 1 — case 1 (FWD) secondary tests (STAGED, not applied)

**Status:** STAGED 2026-05-25. **NEEDS pool13 dynamic-diff validation before apply.**
Branch: `wave3-chain-c-cascade-investigation`. Build at staging: 1,867,936 bytes (no source changed).

## Scope

- **Function:** `td5_track.c::update_position_recursive`, switch `case 1:` at line 2576
- **Case:** 1 (FWD only). Cases 2/4/8 are sister fixes deferred to separate staging files.
- **Orig:** `UpdateActorTrackPosition @ 0x004440f0`, case 1 branch at 0x0044434c-0x004445ad.
- **Closes (partial):** `re/analysis/wave4_deep_audits/da_t2_update_actor_track_pos.md` Fix 1 (D2).
- **Triage row:** `oversight_triage_2026-05-24.md` DA-T2-cascade (HIGH-risk, NEEDS_RUNTIME_TRACE).

## Orig decomp summary (case 1, FWD-only, decompile of 0x004440f0)

After the primary 4-edge crosses produce `bVar8 == 1` (only the FWD edge crossed), orig
re-seeds `bVar10 = 0xf`, and replaces it with `first_mask[type]` when `sub_lane <= 1`
(orig: `uVar16 == 1 || (int)(uVar16 - 1) < 0` — both arms select the masked path).

Then runs a 3-outcome decision tree on two secondary cross-products taken on the
**current span** vertices `psVar2` (e41[sub] — far right) and `psVar5` (e40[sub] — near left):

1. **Test #1** gated on `bVar10 & 2`. Cross of `(psVar2[-3] -> psVar2)` =
   `(e41[sub-1] -> e41[sub])`. Sign check:
   `(prev.x - cur.x)*(pos.z - cur.z) - (prev.z - cur.z)*(pos.x - cur.x) > 0`.
   - If **>0** ⇒ **ADVANCE.** Dispatch by current span_type:
     - **type 10** (SENTINEL_END): `iVar11 = link_next`, `sub_lane += (next.lc - cur.lc)`.
     - **type 8** (JUNCTION_FWD): if `sub_lane < (span_records[span+1].lane_count)` ⇒
       `iVar11 = span_idx + 1` (main, no delta); else `iVar11 = link_next`, apply lane-count delta.
     - **default:** `iVar11 = span_idx + 1`, `sub_lane += (cur.h_offset - new.h_offset)`.
   - Then `t[2]++; t[3] = max(t[2], t[3]); t[0] = new_span; byte[12] = sub_lane - 1; RET`.

2. **Test #2** gated on `bVar10 & 8`. Cross of `(psVar5[-3] -> psVar5)` =
   `(e40[sub-1] -> e40[sub])`, sign formula identical with `psVar5/psVar5[-3]` in
   place of `psVar2/psVar2[-3]`.
   - If **<= 0** (or mask bit 8 clear) ⇒ **STAY.** Just `byte[12] = sub_lane - 1; RET`. **No span change.**
   - If **> 0** ⇒ **STEP BACK by one span.** Dispatch:
     - **type 9** (SENTINEL_START): `t[0] = link_prev`, `t[2]--`,
       `byte[12] = sub_lane + (prev.lc - cur.lc) - 1; RET`.
     - **type 11** (JUNCTION_BWD): if `sub_lane >= span_records[span-1].lane_count` ⇒
       same as type-9 (link_prev branch); else `t[2]--; t[0] = span_idx - 1; byte[12] = sub_lane - 1; RET`.
     - **default:** `t[0] = span_idx - 1`, `t[2]--`, `byte[12] = sub_lane + (cur.h_off - prev.h_off) - 1; RET`.

The port's existing `resolve_neighbor(crossing_bit=0x01)` already encodes the ADVANCE
dispatch (cases 10/8/default) verbatim, and `resolve_neighbor(crossing_bit=0x04)` encodes
the STEP-BACK dispatch (cases 9/11/default). The fix reuses both helpers.

## Port current case-1 behaviour (td5_track.c:2576-2597)

Always-advance. Calls `resolve_neighbor(span, &sub, 0x01, ...)`, bumps `track_state[2]`
+ high-water, writes back new span and `sub_lane - 1`. Returns early on prev_type==8
(junction). **No secondary cross tests; never selects STAY or STEP-BACK.**

This is the root cause documented at td5_track.c:2545-2568 ("[DA-T2 audit 2026-05-22 — pending application]").

## Diff (apply this to td5_track.c, **do not apply yet**)

Replace the entire `case 1:` block (lines 2576-2597) with:

```diff
@@ td5_track.c:2576-2597 — case 1 (FWD)
-        case 1: { /* Forward */
-            int prev_type = s_span_array[span_idx].span_type;
-            new_span = resolve_neighbor(span_idx, &sub_lane, 0x01, pos_x, pos_z);
-            span_idx = new_span;
-            track_state[0] = (int16_t)new_span;
-            track_state[2]++;
-            if (track_state[2] > track_state[3])
-                track_state[3] = track_state[2];
-            /* Original appends -1 to sub_lane post-step unconditionally,
-             * including for junction (type 8) transitions.
-             * [CONFIRMED @ 0x004440F0 case 0x01: `cVar15 + ... + (-1)`]
-             * resolve_neighbor now uses the running sub_lane (not geometric
-             * projection) so the -1 applies cleanly.  The early return keeps
-             * single-step semantics matching the original — negative stored
-             * values are clamped to 0 at the next call's walker entry. */
-            sub_lane -= 1;
-            if (prev_type == 8) {
-                ((int8_t *)track_state)[12] = (int8_t)sub_lane;
-                return;
-            }
-            break;
-        }
+        case 1: { /* Forward — 3-outcome secondary-cross [CONFIRMED @ 0x0044434c-0x004445ad]
+                   *
+                   * Re-seed edge mask, then test two secondary crosses on the
+                   * CURRENT span's column-pair vertices at (sub-1, sub):
+                   *   Test #1 (mask & 2): cross(e41[sub-1] → e41[sub])  > 0 → ADVANCE + sub--
+                   *   Test #2 (mask & 8): cross(e40[sub-1] → e40[sub])  > 0 → STEP-BACK + sub--
+                   *                       else                                 → STAY  + sub--
+                   * Test #1 uses resolve_neighbor(0x01) for ADVANCE dispatch (matches
+                   * orig type 10/8/default branches at 0x00444399-0x004443DC).
+                   * Test #2 uses resolve_neighbor(0x04) for STEP-BACK dispatch (matches
+                   * orig type 9/11/default branches at 0x0044454F-0x004445A6). */
+            TD5_StripSpan *sp = &s_span_array[span_idx];
+            int type = sp->span_type;
+            if (type < 0 || type > 11) type = 0;  /* safe-table */
+            int e40_base = (int)sp->left_vertex_index  + k_quad_vertex_offsets[type][0];
+            int e41_base = (int)sp->right_vertex_index + k_quad_vertex_offsets[type][1];
+            /* Orig 0x00444356: bVar10 = 0xf; if (sub == 1 || sub - 1 < 0) use first_mask.
+             * Both arms (==1 and <1) take the masked path → `sub_lane <= 1`. */
+            uint8_t mask = (sub_lane <= 1) ? s_edge_mask_first[type] : 0x0f;
+
+            int64_t cross_adv = 0;
+            if (mask & 0x02) {
+                /* orig: ((psVar2[-3] - psVar2) × (pos - psVar2)) > 0 ;
+                 * compound_cross(sp, a, b) = (b - a) × (pos - a).
+                 * Map a=cur=e41[sub], b=prev=e41[sub-1] → equivalent sign. */
+                cross_adv = compound_cross(sp,
+                                           e41_base + sub_lane,
+                                           e41_base + sub_lane - 1,
+                                           pos_x, pos_z);
+            }
+            if ((mask & 0x02) && cross_adv > 0) {
+                /* ADVANCE — same dispatch as the prior always-advance path. */
+                int prev_type = sp->span_type;
+                new_span = resolve_neighbor(span_idx, &sub_lane, 0x01, pos_x, pos_z);
+                span_idx = new_span;
+                track_state[0] = (int16_t)new_span;
+                track_state[2]++;
+                if (track_state[2] > track_state[3])
+                    track_state[3] = track_state[2];
+                sub_lane -= 1;
+                ((int8_t *)track_state)[12] = (int8_t)sub_lane;
+                /* Orig RETs unconditionally on ADVANCE — keep the early-exit for
+                 * type 8 to preserve the prior junction-route semantics, then
+                 * also early-exit for non-junction (orig case 1 is single-pass). */
+                if (prev_type == 8) return;
+                return;
+            }
+
+            int64_t cross_back = 0;
+            if (mask & 0x08) {
+                /* orig: ((psVar5 - psVar5[-3]) × (pos - psVar5[-3])) > 0 ;
+                 * compound_cross(sp, a, b) = (b - a) × (pos - a).
+                 * Map a=prev=e40[sub-1], b=cur=e40[sub] → equivalent sign. */
+                cross_back = compound_cross(sp,
+                                            e40_base + sub_lane - 1,
+                                            e40_base + sub_lane,
+                                            pos_x, pos_z);
+            }
+            if ((mask & 0x08) == 0 || cross_back <= 0) {
+                /* STAY — no span change, just decrement sub_lane.
+                 * Orig: `*(char*)(track_state + 6) = cVar15 + -1; return;` */
+                sub_lane -= 1;
+                ((int8_t *)track_state)[12] = (int8_t)sub_lane;
+                return;
+            }
+
+            /* STEP BACK — span -= 1 (or type 9/11 link), t[2]--, sub_lane--.
+             * resolve_neighbor(0x04) encodes the type 9/11/default dispatch and
+             * applies the appropriate sub_lane delta. */
+            new_span = resolve_neighbor(span_idx, &sub_lane, 0x04, pos_x, pos_z);
+            span_idx = new_span;
+            track_state[0] = (int16_t)new_span;
+            track_state[2]--;
+            sub_lane -= 1;
+            ((int8_t *)track_state)[12] = (int8_t)sub_lane;
+            return;
+        }
```

**Diff size:** ~22 lines removed, ~78 lines added (net +56 lines).

### Indentation / style notes
- Uses 4-space indent matching surrounding code.
- Brace style matches case 3 (line 2618-2669): `case N: { ... break; }`.
- All three outcome paths `return` (not `break`) — mirrors orig's terminal RETs in
  every case-1 branch. The compound cases already do `break` because they share a
  post-switch writeback (line 2904 `((int8_t *)track_state)[12] = (int8_t)sub_lane;`).
  case 1 must NOT fall into that writeback because each outcome has its own custom
  sub_lane formula already applied — falling through would be a double-write.
- ADVANCE outcome's "prev_type == 8 then return; else return;" collapses to a single
  `return`; the conditional is kept for diff-clarity vs the prior code (and to make
  the intent explicit: orig RETs unconditionally here regardless of type 8).

## Required helpers (verified present in port)

| Helper | Site | Status |
|---|---|---|
| `s_span_array[]` | td5_track.c:139 | exists |
| `k_quad_vertex_offsets[12][2]` | td5_track.c:1578 | exists, byte-faithful w/ DAT_00474e40/41 |
| `s_edge_mask_first[12]` | td5_track.c:100 | exists, byte-faithful w/ DAT_00474e28 |
| `compound_cross(sp, va_idx, vb_idx, pos_x, pos_z)` | td5_track.c:2482 | exists, returns `(b - a) × (pos - a)` |
| `resolve_neighbor(span, &sub, crossing_bit, pos_x, pos_z)` | td5_track.c:2223 | exists; 0x01 = FWD-advance dispatch, 0x04 = BACK-advance dispatch — both already encode the type 8/9/10/11/default branches matching orig case 1. |

**No new helpers required.** No header changes. No new globals. No build-system changes.

## Risk + validation plan

### Risk class
**HIGH cascade impact.** Changes the per-tick span-update behaviour for every actor
when `bits == 1` (single FWD edge cross). Triggered on virtually every straight-line
forward-driving tick. Audit estimate (Section D, D2): 10-30 fields lift at sub_tick=1.
Mis-application risk: if STAY/STEP-BACK fire when orig would ADVANCE (or vice versa),
slot 0 bias residue can balloon from ~210u to thousands; AI cars can land on the wrong
span and trigger the brute-force fallback at line 2926.

### Pre-apply gate

1. Snapshot pool13 dynamic-diff baseline:
   ```
   python tools/run_pool13_diff.py --track Edinburgh --racers 6 --ticks 200 \
       --out re/analysis/pool13_baseline_pre_da_t2_fix1.csv
   ```
   Record slot 0 lat_offset_bias residue (current: ~210u per `reference_fwd_track_comp_sqrt_divisor_2026-05-21`).

2. Build current binary, confirm 1,867,936 bytes.

### Apply

Apply the diff. Rebuild. Confirm:
- compile clean (no new warnings; the new locals `e40_base`/`e41_base`/`mask`/`type`
  shadow nothing in the enclosing scope — the iter loop's locals don't reuse these names).
- binary size delta should be < 1 KB (pure C insertion, no new symbols).

### Post-apply measurement

Re-run pool13:
```
python tools/run_pool13_diff.py --track Edinburgh --racers 6 --ticks 200 \
    --out re/analysis/pool13_post_da_t2_fix1.csv
python tools/diff_pool13_csv.py \
    re/analysis/pool13_baseline_pre_da_t2_fix1.csv \
    re/analysis/pool13_post_da_t2_fix1.csv
```

### Acceptance

- **PASS:** residual bias slot 0 drops (target: any reduction; ideal: <100u).
  No slot's wall_hits or lat_offset_bias regresses by >2x the baseline.
- **REGRESSION REVERT:** if any slot's wall_hits or peer_push_count grows >3x
  the baseline, revert. Suspect signs:
  - signed cross-product direction inverted (most likely: swapped a/b in
    `compound_cross` call) ⇒ produces ALWAYS-STAY (cars never advance spans on
    straight roads ⇒ lap timing breaks immediately, visible within ~5 sec of race start).
  - mask polarity wrong ⇒ subtle: produces wrong STAY frequency, hard to see
    without diff.

### Frida hook (TODO — staging file draft, not yet shipped)

`tools/_probes/da_t2_case1_probe.js`:

```javascript
// Hook orig UpdateActorTrackPosition @ 0x004440f0; log (track_state ptr, span_idx,
// sub_lane, bits, outcome) for case 1 specifically. Capture which branch fires
// (ADVANCE / STAY / STEP-BACK) so port can be paired against a known orig trace.
//
// Anchor: address of bVar8 switch (after case 1 entry but before bVar10 reseed),
//   inferred at ~0x0044434c. The case 1 branch is reached by JMP from the switch
//   table at 0x00445420.
//
// Outcome distinguishers (within case 1):
//   RET at 0x004443DC area (ADVANCE early-RET)
//   RET at 0x00444443 area (STAY, type-detect default)
//   RET at 0x004444A8 / 0x00444504 / 0x004445A6 (STEP-BACK terminal RETs)
//
// Record (span, sub, mask, cross1, cross2, branch_taken) for offline diff vs port.
//
// Drop into tools/_probes/, queue via orig_probe_queue.py.
```

To be authored as a follow-up if a paired diff is needed. The acceptance test
above is gateable on pool13 CSV alone for a first pass.

## Sequence after Fix 1

Per audit doc Section E ("Sequencing: apply Fix 1+3 first → measure pool13 lift →
Fix 2+4 → Fix 5 after Frida histogram"):

1. **(this staging)** Fix 1 — case 1 (FWD) secondary tests.
2. Fix 3 — case 2 (RIGHT) post-step retests on the new span (separate staging).
   - Orig 0x004445ae-0x004449c0. Per audit doc Section E Fix 3.
3. Measure pool13 lift for 1+3 together.
4. Fix 2 — case 4 (BACK) secondary tests, symmetric to Fix 1.
   - Orig 0x004449c1-0x00444c4b. Unblocks `todo-reverse-not-triggered`.
5. Fix 4 — case 8 (LEFT) post-step retests, symmetric to Fix 3.
   - Orig 0x00444e10-0x00445031.
6. Fix 5 — default no-op for bit patterns 5/7/10/11.
   - Only after Frida histogram confirms patterns occur in practice (audit doc D10).

## Notable decisions / open questions

- **Sign convention of `compound_cross` vs orig:** orig uses `(prev - cur) × (pos - cur)`
  for Test #1 and `(cur - prev) × (pos - prev)` for Test #2. Port's `compound_cross(sp, a, b)`
  computes `(b - a) × (pos - a)`. For Test #1 set `a=e41[sub], b=e41[sub-1]` →
  `(prev - cur) × (pos - cur)`. For Test #2 set `a=e40[sub-1], b=e40[sub]` →
  `(cur - prev) × (pos - prev)`. Both algebraically identical to orig.
  Verify in code review.

- **`compound_cross` clamps negative or out-of-range vertex indices:** at line 2491
  it calls `vertex_at(v_a_idx)` which is `&s_vertex_table[index]` with no bounds check.
  For `sub_lane == 0` (mask path), `e41_base + sub_lane - 1 = e41_base - 1` which
  could underflow if `e41_base == 0`. The audit doc's pseudocode and orig both rely
  on the mask gate `(sub_lane <= 1) ? first_mask : 0xf` to suppress the test when
  the mask bit is 0. Verify `first_mask[type] & 0x02 == 0` for all types where
  underflow can happen — looking at `s_edge_mask_first[12]`:
  `{0x00, 0x0E, 0x0E, 0x06, 0x06, 0x0E, 0x0C, 0x0C, 0x0E, 0x0E, 0x0E, 0x0E}`.
  Bit 2 (0x02) is set for **all types except 0 and 6/7** when `sub_lane <= 1`.
  So when `sub_lane == 0`, the cross IS evaluated for most types and reads
  `vertex_at(e41_base - 1)`. **This matches orig behaviour exactly** (orig reads
  `psVar2[-3]` unguarded), so any underflow is shared with orig — not a port bug
  introduced by this fix. Note for code reviewer: confirm `s_vertex_table` is in
  a heap region that tolerates one-vertex underflow reads (it's a `static *` pointing
  into the STRIP.DAT blob — preceding bytes are the strip-record array, so a stray
  read returns whatever those bytes encode, byte-equivalent to orig's stray read).

- **Single-pass vs loop:** Orig case 1 always RETs (never falls through to the
  next iter). The port's surrounding `for (iter < TRACK_MAX_RECURSION)` loop
  exits naturally because we `return` from all three outcome paths — this is
  consistent with the compound cases 3/6/9/12 that also have terminal `break`
  patterns post-step. Confirmed safe.

- **Interaction with `single_step` and `compound_done`:** Neither flag is touched
  by case 1 in this fix (they only matter for the inter-iter loop control which
  we bypass via `return`). The audit doc's outer-loop notes apply unchanged.

## TODOs / questions left

- [ ] Confirm pool13 dynamic-diff tool name (assumed `tools/run_pool13_diff.py`,
      not verified). If named differently, update the acceptance section.
- [ ] Author `tools/_probes/da_t2_case1_probe.js` (drafted in this doc; not yet committed).
- [ ] Cross-check `s_vertex_table` underflow safety in a real load — if it overlaps
      the strip-record array, no harm; if it lands at a heap-block boundary the
      read could fault under PageHeap. Run once with appverif full PageHeap on
      Edinburgh to confirm (per `reference_pageheap_loadstatictracktextureheader_2026-05-20`
      methodology).
- [ ] If applying alongside Fix 3 (case 2 staging not yet written), apply both in
      the same commit per audit doc sequencing.

## References

- `re/analysis/wave4_deep_audits/da_t2_update_actor_track_pos.md` — full audit, Section E Fix 1.
- `re/analysis/oversight_triage_2026-05-24.md` — DA-T2-cascade row, NEEDS_RUNTIME_TRACE class.
- `reference_pool13_dynamic_diff_2026-05-21` — paired-diff baseline (+35u/tick peer-push).
- `reference_fwd_track_comp_sqrt_divisor_2026-05-21` — ~210u residual slot 0 bias.
- Orig decomp captured 2026-05-25 from master `TD5.gpr` Ghidra project (read-only
  session `d0b183fe…`, released at exit).
