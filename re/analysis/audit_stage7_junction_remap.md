# Audit: Stage 7 peer-cmp polarity + junction-remap no-match sentinel

Read-only static audit against TD5_d3d.exe (Ghidra `read_only=true`,
project `TD5_pool8`).

Worktree: `.claude/worktrees/audit-stage7-junction-remap`
Branch: `audit-stage7-junction-remap`
Base: `master` @ `8af496f`

---

## Task A — Stage 7 peer-cmp polarity asymmetry

### Source-of-truth (Ghidra listing)

Function: `UpdateTrafficRoutePlan` @ `0x00435E80`

Two peer-compare blocks straddle the polarity gate:

| Polarity branch | Site                | Compare                                                          |
|-----------------|---------------------|------------------------------------------------------------------|
| `polarity == 0` | `0x00436448-0x436456` | `CALL FindNearestRoutePeer; CMP EAX, [ESP+0x40]` — peer vs `param_1` (slot index passed in) |
| `polarity != 0` | `0x004365A6-0x4365B4` | `CALL FindNearestRoutePeer; MOV ECX,[EBX+0xD4]; CMP EAX, ECX` — peer vs `ref_slot` from `rs[+0xD4]` |

`EBX` in this function holds `&gActorRouteStateTable[param_1*0x47]`
(`rs` for the current traffic actor). The asymmetry is real in the listing.

### Writers of `rs[+0xD4]` (RS_SLOT_INDEX)

`search_instructions` over the whole image found exactly **one** writer
into the route-state struct at offset `+0xD4`:

- `0x004334F7  MOV [EAX+0xD4], ESI` inside `InitializeRaceActorRuntime`
  (`0x00432E60`). Decompiled as `local_4[0x35] = local_c;` where
  `local_c` is the per-iteration **slot index** (0 .. g_racerCount-1).

The other hit (`0x00458CB5` in `DecodeJPEGMCU`) writes into an unrelated
JPEG context struct — not the route-state table.

### Conclusion (A)

`rs[+0xD4] == slot` is an **init-time invariant** for every actor slot
(both player/AI racers 0-5 and traffic 6-11). No code path mutates
`+0xD4` after `InitializeRaceActorRuntime` returns. For the traffic
slots that `UpdateTrafficRoutePlan` operates on, comparing the peer
return against `param_1` is **bit-identical** to comparing against
`[EBX+0xD4]`. The port's simplification (`peer == slot`) is **byte-
faithful in observable behavior**.

The two listing sites are structurally distinct (different polarity
branches, different operand sources) but functionally equivalent given
the invariant. The asymmetry in the original is almost certainly a
compiler artifact of separate `__cdecl` peer-compare blocks merging
against locally-cached registers (the polarity!=0 path keeps EBX live
through the call, the polarity==0 path uses the stack-spilled `param_1`).

**Verdict: FAITHFUL — document and forget.**

Recommended action: keep the existing `peer == slot` simplification.
Add a one-line comment near the polarity!=0 branch in
`UpdateTrafficRoutePlan` port noting the invariant `rs[+0xD4] == slot`
(seeded once in `InitializeRaceActorRuntime`, never rewritten) so a
future audit doesn't re-flag the divergence. Source: this report.

---

## Task B — Junction-remap no-match sentinel

### Source-of-truth (Ghidra listing)

Function: `InitializeTrafficActorsFromQueue` @ `0x00435940`,
REMAP path (route selector = 1).

The inlined walker at `0x004359E5-0x00435A2B` iterates the junction
remap table at `DAT_004c3da0+0x1c` (stride 6). Decompiled:

```c
local_1c = 0;
if (0 < jump_entry_count) {
  do {
    if ((range_lo <= queue_span) &&
        (queue_span <= (range_hi))) {
      sVar14 = (remap_dst - range_lo) + queue_span;
      goto LAB_00435a2b;           // match path
    }
    local_1c++;
    puVar11 += 3;
  } while (local_1c < jump_entry_count);
}
sVar14 = -1;                       // no-match sentinel
LAB_00435a2b:
*psVar13 = sVar14;                 // actor.span_raw = sVar14
```

Confirmed: no-match writes **-1** into `actor.span_raw`, which then
flows into `InitActorTrackSegmentPlacement(&span_raw, &world_pos)`.

### Port's helper (`td5_track_apply_target_span_remap`)

Returns the wrapped input `lin_span` on no-match, never -1. The port
in `td5_ai.c:3718` therefore added a heuristic:

```c
remapped_int = td5_track_apply_target_span_remap((int)queue_span, 0);
if (remapped_int == (int)queue_span)
    remapped_span = (int16_t)-1;     // synth sentinel
else
    remapped_span = (int16_t)remapped_int;
```

### Divergence boundary

The heuristic mis-fires only when a junction entry has
`remap_dst == range_lo` (identity-remap) **and** the queue span lands
inside that entry's range. The original would treat that as a match
(`sVar14 = queue_span`), the port treats it as a miss (`sVar14 = -1`).

Identity-remap entries are not expected on shipped tracks — the
junction table is the LEFT/RIGHT branch fork-up table, where
`remap_dst != range_lo` by construction (every entry is a non-trivial
redirect). The opposite failure mode — mis-configured `TRAFFIC.BUS`
queue records on truly non-branching spans — is the one the existing
port comment flags.

### Conclusion (B)

For shipped track data, the port and the listing produce identical
`actor.span_raw` values out of the REMAP path. The port's heuristic
collapses two distinct cases into the same sentinel (true-miss and
identity-match), but neither case occurs in shipped data.

**Verdict: FAITHFUL FOR SHIPPED DATA — document and forget.**

The existing in-source comment (`td5_ai.c:3699-3712`) already documents
this; the audit confirms its claim.

Recommended action: no code change. If a future track ships
identity-remap entries OR malformed `TRAFFIC.BUS` records, the cleanest
fix is to give `td5_track_apply_target_span_remap` an optional
out-parameter signalling match/miss so the caller can distinguish
identity-remap from no-match without the equality heuristic. Defer
until evidence of breakage.

---

## Pool slot

- Acquired: `TD5_pool8` via `ghidra_pool/TD5_pool8.assigned`
- Read-only: `project_program_open_existing(..., read_only=true)`
- Released after audit: `program_close` + `rm TD5_pool8.assigned`

## Summary

| Task | Verdict | Action |
|------|---------|--------|
| A — peer-cmp polarity asymmetry | FAITHFUL (invariant holds) | none / 1-line comment |
| B — junction-remap no-match sentinel | FAITHFUL for shipped data | none (already documented) |
