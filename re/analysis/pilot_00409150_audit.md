# Pilot Audit — 0x00409150 ResolveVehicleContacts

**Date:** 2026-05-14
**Pool slot:** TD5_pool9
**Worktree:** `.claude/worktrees/precise-00409150` on branch `precise-00409150` (from master)
**Tag:** `pool9_00409150`
**Port-side function:** `td5_physics_resolve_vehicle_contacts` @ `td5_physics.c:3175`
**Caller:** `RunRaceFrame (0x0042B580)` — single caller, once per sim_tick.
**Callees:** `ResolveVehicleCollisionPair (0x00408A60)` (pool15 territory) and `ResolveSimpleActorSeparation (0x00408F70)` (separate function, out of scope).
**Body:** `0x00409150..0x004092CD` = 381 bytes / 121 instructions.

## Function structure (from listing)

```
PHASE 1 — Build broadphase grid + bounds entries
  for slot in 0..racer_count:
    cardef = actor->car_definition_ptr               ; field +0x1b8 (loaded via piVar13[-0x11])
    radius = (int32)(int16)cardef[+0x80]
    span   = (int16)actor->track_span_normalized     ; field +0x80
    bucket = (span >> 2)                             ; PLAIN SAR, NO wrap-mask
    bounds[slot].xMin = (actor->world_pos.x >> 8) - radius     ; [0x483050 + slot*0x14 + 0x00]
    bounds[slot].xMax = (actor->world_pos.x >> 8) + radius     ; [0x483050 + slot*0x14 + 0x08]
    bounds[slot].zMin = (actor->world_pos.z >> 8) - radius     ; [0x483050 + slot*0x14 + 0x04]
    bounds[slot].zMax = (actor->world_pos.z >> 8) + radius     ; [0x483050 + slot*0x14 + 0x0C]
    bounds[slot].chain = grid[bucket + 1]                      ; previous head, byte (= [0x483060 + slot*0x14])
    grid[bucket + 1]   = (byte)slot                            ; new head (in gBroadphaseSpatialGrid)

PHASE 2 — Outer loop over slotA, inner: walk 3 grid buckets, walk chain
  for slotA in 0..racer_count:
    base = (actorA->track_span_normalized >> 2)
    pbVar = &grid[base]              ; note: starts at base, NOT base+1 (Phase 1 inserted at base+1)
    bucket_remaining = 3
    do:
      slotB = *pbVar                 ; read 1 byte = head of bucket
      walk_iter = 0
      do:
        if (slotB == 0xFF) break
        if (slotA < slotB && slotB < racer_count):
          if (actorA->vehicle_mode == 0 &&
              actorA->wheel_contact_bitmask < 0x0F &&
              actorB->vehicle_mode == 0 &&
              actorB->wheel_contact_bitmask < 0x0F):
            ResolveVehicleCollisionPair(slotA, slotB)
          else:
            ResolveSimpleActorSeparation(slotA, slotB)
        slotB = bounds[slotB].chain   ; (uint8) [0x483060 + slotB*0x14]
        walk_iter++
      while walk_iter < 0x11          ; max 17 chain walks per bucket
      pbVar++                         ; next bucket
      bucket_remaining--
    while bucket_remaining != 0

CLEANUP — reset grid + bounds chains (only slots that were used)
  for slot in 0..racer_count:
    bounds[slot].chain = 0xFF
    grid[(actorA->track_span_normalized >> 2) + 1] = 0xFF
```

## Key data symbols (TD5_d3d.exe, image base 0x00400000)

| Symbol | Address | Purpose |
|---|---|---|
| `gBroadphaseActorBoundsTable` | 0x00483058 (xMax of slot 0; entries start at 0x00483050) | 16-slot × 0x14 stride; fields {xMin, zMin, xMax, zMax, chain_byte, _pad[7]} |
| `gBroadphaseActorChainLinks` | 0x00483060 | Alias for bounds[0].chain — same storage as bounds table chain bytes |
| `gBroadphaseSpatialGrid` | 0x00483554 | Per-bucket head array; insert/walk uses `(span>>2) + 1` (Phase 1) or `(span>>2) + offset` (Phase 2 walks 3 consecutive cells) |
| `g_racerCount` | 0x004AAF00 | int32 — total active racers/AI/traffic |
| `g_actorRuntimeState` | 0x004AB108 | Base of actor array, 0x388 stride |

Grid stride is per-byte; bucket index = `(span >> 2) + 1` (Phase 1 insert).

The +1 in Phase 1 insert vs Phase 2 walk-base of (span>>2) means **slot's own bucket = base+1**, and the 3 walked buckets are `[base, base+1, base+2]` = `[my-1, my, my+1]` in own-bucket relative coordinates.

## Bucket walk geometry

Phase 1 inserts slot at `grid[base+1]` where `base = (actor->track_span_normalized >> 2)`.
Phase 2 walks 3 consecutive grid bytes starting at `grid[base]` (where `base = current actorA's span >> 2`).

So for slotA, the walked buckets are:
- `grid[base_A + 0]` (offset 0 from base)
- `grid[base_A + 1]` (offset 1 — slotA was inserted HERE)
- `grid[base_A + 2]` (offset 2)

Since Phase 1 inserted slot i at `grid[base_i + 1]`, the buckets walked by slotA include slots whose insert-bucket falls at base_A, base_A+1, or base_A+2 — i.e. slots with `base_i ∈ {base_A-1, base_A, base_A+1}` (because slot inserted at `base_i + 1` is found by walker reading `grid[base_A + k]` iff `base_i + 1 == base_A + k` ⇒ `base_i = base_A + k - 1` ⇒ `base_i ∈ {base_A-1, base_A, base_A+1}`).

Net: bucket span window is **±1 bucket** (i.e. span difference <=4 between A and B will surface them). Port's `boff in [-1, 0, +1]` matches the same window.

## Confirmed divergences (port vs original)

### D1 — Port masks bucket index `& 0xFF`, original does NOT **(POTENTIAL HIGH IMPACT)**

Original at 0x00409197-9B:
```
MOVSX ECX, word [ESI + 0xfffffe84]   ; ECX = actor->track_span_normalized (int16)
SAR ECX, 0x2                          ; ECX = span >> 2  (plain arithmetic SAR, sign-extending)
INC ECX                               ; ECX = bucket+1 (the "+1" indexing convention)
```
Then `MOV [ECX + 0x483554], BL` — writes to `grid[(span>>2) + 1]`.

Port at td5_physics.c:3239:
```c
int bucket = (seg >> 2) & (COLLISION_GRID_SIZE - 1);   /* mod 256 */
```

**Behavioral difference:**
- For tracks where `(span >> 2)` stays in [0, 254] (= span 0..1019), behavior is identical.
- For longer tracks where span > 1023 (rare but happens on Edinburgh and circuits), port wraps to a low bucket while original addresses a higher grid slot.
- Wrap **collapses spatially-distant actors into the same bucket** — port over-approximates V2V tests.

Original's grid size is implicit (the binary's BSS allocation); it appears large (~4 KB at 0x00483554 with no symbol before the next BSS). The grid being indexed without wrap means the original ASSUMES enough grid slots exist. For race tracks the span is bounded by track length, which fits.

**Fix:** Either grow `COLLISION_GRID_SIZE` to match the binary (~4096) OR keep wrap but accept the over-approximation (currently the case). The wrap path produces spurious pair-tests; correctness-preserving but performance-affecting and **changes byte-for-byte pair iteration order** because wrapped buckets may contain slots in different chain positions than the original.

### D2 — Port pre-zeros AABB for null-cardef slots, original would crash on null **(NO-OP)**

Port at td5_physics.c:3224-3227:
```c
if (!actor->car_definition_ptr) {
    memset(g_actor_aabb[i], 0, sizeof(g_actor_aabb[i]));
    continue;
}
```
Original at 0x0040916D-A4 dereferences `cardef[+0x80]` unconditionally. If `cardef == NULL`, original SIGSEGVs. Port's defensive branch is an enhancement that activates ONLY for slots `0..racer_count-1` whose cardef is null — never in normal play.

**No fix required.** Document only.

### D3 — Port uses upfront full `memset(grid, 0xFF, 256)`, original uses per-actor cleanup **(EFFECTIVELY NO-OP)**

Original cleanup at 0x004092A1-C4 resets each used slot's grid cell + bounds chain to 0xFF. Port resets the entire 256-byte grid via `memset` at function entry (line 3218).

In steady state (after first call), both produce the same starting state. **No fix required** — the memset is faster and cleaner.

### D4 — Port also checks `actor->car_definition_ptr` in Phase 2 **(NO-OP)**

Port at td5_physics.c:3250 and :3274 adds `if (!a->car_definition_ptr) continue;` / `if (!b->car_definition_ptr) continue;`. Original has no such check. Equivalent in practice — once Phase 1 has built the chain, only valid actors are in the chain.

**No fix required.** Document only.

### D5 — Dispatch gate **(MATCH)**

Port at td5_physics.c:3287-3290:
```c
int a_scripted = (a->vehicle_mode != 0) ||
                 (a->wheel_contact_bitmask >= 0x0F);
int b_scripted = (b->vehicle_mode != 0) ||
                 (b->wheel_contact_bitmask >= 0x0F);
if (a_scripted || b_scripted) collision_detect_simple(a, b);
else                          collision_detect_full(a, b, i, j);
```

Original at 0x00409212-3F:
```
CMP byte [EBX], 0           ; actorA->vehicle_mode == 0?
JNZ 0x40924a                ; → simple branch
CMP byte [EBX+3], 0xf       ; actorA->wheel_contact_bitmask < 0xf?
JNC 0x40924a                ; → simple branch (JNC = "jump if no carry" = >=)
...same for actorB...
CALL 0x00408a60             ; ResolveVehicleCollisionPair (full OBB)
JMP 0x00409251
0x40924a:
CALL 0x00408f70             ; ResolveSimpleActorSeparation
```

Algebraically identical:  `(modeA != 0 || wcbA >= 0x0F || modeB != 0 || wcbB >= 0x0F) → simple`, else `→ full`.

**No fix required.** Confirmed faithful.

### D6 — Pair-order skip mechanic **(MATCH)**

Original at 0x0040920A-10:
```
CMP ESI, EDI                 ; slotB vs slotA
JLE 0x00409254              ; falls through to chain-advance if slotB <= slotA
CMP ESI, EDX                 ; slotB vs racer_count
JGE 0x00409254              ; falls through to chain-advance if slotB >= racer_count
```

Port at td5_physics.c:3267-3270:
```c
if (j <= i) {
    chain = g_actor_aabb[j][4] & 0xFF;
    continue;
}
```

Plus implicit `j < racer_count` via `if (!b->car_definition_ptr) continue` — but only because slots beyond `total` have null cardef. **For correctness-strict parity**, port should ALSO check `j < total` since racer_count could differ from total_actor_count. In practice the chain-walker only contains slots that Phase 1 inserted (i.e. valid actors), so j ≥ total cannot occur.

**No fix required** in normal runs. Could be a corner case under traffic actor lifecycle.

### D7 — Chain walk max-iteration cap **(MATCH)**

Original at 0x00409261: `CMP EBP, 0x10 / JG 0x00409270`. So `iter++` then exit if `iter > 16` (i.e. up to 17 iterations including iter=16). With `iter` starting at 0 and `INC EBP` after each test, max walks = 17. Port `COLLISION_MAX_WALK=17`. **Match.**

### D8 — Phase 1 chain insertion vs Port's separate g_actor_aabb[][4] **(SEMANTIC MATCH)**

Original stores bounds[slot].chain inline with the bounds entry at offset +8 of the 0x14-stride record. Port stores chain in `g_actor_aabb[i][4]` (a separate 32-bit int per slot). Same logical chain, different storage layout. Port's storage is wider (32-bit vs 8-bit), but only the low byte is used (port writes `(uint8_t)i` and reads `& 0xFF`).

**No fix required.**

### D9 — Outer loop variable: `racer_count` snapshot **(POTENTIAL MISMATCH)**

Original re-reads `g_racerCount` after every `ResolveVehicleCollisionPair` / `ResolveSimpleActorSeparation` call (via `MOV EDX, [0x004aaf00]` at 0x00409204 and 0x00409270). Port snapshots `total = td5_game_get_total_actor_count()` ONCE at function entry (line 3186) and uses that throughout.

**Behavioral difference:** if a collision callback decrements/increments `g_racerCount`, the original notices on the next iteration, the port does not. In practice, collision handlers don't modify the racer count, so this is hypothetical.

**No fix required** in normal runs. Document.

### D10 — Outer loop variable `racer_count` is `g_racerCount`, NOT `total_actor_count` **(POTENTIAL HIGH IMPACT)**

This is the most important divergence.

Original uses `g_racerCount` (at 0x004AAF00) as the iteration bound for **both Phase 1 and Phase 2**. This is the count of RACER vehicles (1 + AI cars, e.g. 6). Traffic actors (slots 6-11) are **NOT iterated** by this function — they are left out of the broadphase.

Port at td5_physics.c:3186:
```c
total = td5_game_get_total_actor_count();
if (total > TD5_MAX_TOTAL_ACTORS) total = TD5_MAX_TOTAL_ACTORS;
```

`td5_game_get_total_actor_count()` returns the total actor count (racers + traffic, up to 12). So the port **iterates over all actors including traffic**, while the original iterates only racers.

This is HUGE — V2V collision between racers and traffic exists in the port but **not in the original** here. The original handles traffic collision via separate code paths (V2V at the per-actor update step, not the broadphase).

Let me verify with the upstream caller:

`RunRaceFrame (0x0042B580)` calls `ResolveVehicleContacts()` once per sim tick. The original's traffic-V2V handling is in `UpdateTrafficActorMotion` (per-traffic-actor inline checks) — not in this broadphase function.

**Fix candidate:** Replace `td5_game_get_total_actor_count()` with `g_racer_count` (the count of racing vehicles, NOT including traffic). This will cause traffic actors to no longer be broadphase-included.

Risk: known memory entries reference "Traffic (slots 6-11) runs the FULL OBB path in the original — `UpdateTrafficActorMotion`'s cVar3==0 branch leaves field_0x379 at 0. Gating on slot_index was wrong." That comment in `td5_physics.c:3284-3286` (the dispatch comment) is correct insofar as TRAFFIC vs RACER in the dispatch gate. But the iteration bound is `g_racerCount`, so traffic actors are EXCLUDED from this broadphase entirely. The dispatch gate applies only to slots that ARE iterated.

So: port's `total = total_actor_count` iterates traffic. The original's `racer_count` doesn't. The dispatch-comment's claim about traffic going through FULL OBB applies only IF the iteration loop includes traffic, which the original's doesn't.

**The port's behavior is an enhancement** (broadphase covers traffic too). To be byte-faithful, change iteration bound to `g_racer_count`.

### D11 — Grid resolution: `(span >> 2)` is plain SAR, no rounding-toward-zero idiom **(MATCH)**

Listing at 0x00409197-99 and 0x004091dd-df: `SAR EAX, 2` plain — no CDQ/AND/ADD round-to-zero pattern. Port uses `seg >> 2` which is C arithmetic right shift = same semantics for non-negative `seg`. Port clamps `seg < 0 → seg = 0` (line 3238) which the original DOES NOT — but the `MOVSX` reads track_span_normalized as signed int16, so negative spans would `SAR -1, 2 = -1` which becomes grid index `0` after `+1`. Port's clamp produces grid index `0` after `(0 >> 2) & 0xFF = 0`. The +1 in original's index means original would write to grid[0] for span=-1..-4 — different from port's grid[0].

**Subtle:** port's clamp `if (seg < 0) seg = 0;` produces `bucket = 0`. Original produces `bucket = (span>>2) + 1 = 0` (for span = -1) or `bucket = -1 (i.e. 0xFFFFFFFF)` (for span = -4 → wraps to enormous index, OOB write).

In practice, `track_span_normalized` is always ≥ 0 after Phase 0 (track_setup), so this is a phantom divergence.

**No fix required** in normal runs.

## Audit summary

| # | Divergence | Impact | Fix needed |
|---|---|---|---|
| D1  | Port `& 0xFF` bucket mask vs no-mask original | wrap-induced bucket collision on long tracks | Optional (grow grid to 1024+) |
| D2  | Port null-cardef guard | none (defensive) | No |
| D3  | Port upfront memset vs per-slot cleanup | none (steady-state equivalent) | No |
| D4  | Port phase-2 null-cardef guard | none | No |
| D5  | Dispatch gate | none — match | No |
| D6  | `j <= i` continue vs `JLE` skip-pair-but-walk | none — match | No |
| D7  | Chain walk max 17 iters | none — match | No |
| D8  | Chain storage in g_actor_aabb[][4] vs bounds entry | none — semantic match | No |
| D9  | Outer-loop racer_count re-read each iter | hypothetical only | No |
| **D10** | **`total_actor_count` vs `g_racer_count` iteration bound** | **HIGH — includes traffic actors in broadphase, original doesn't** | **Yes — switch to g_racer_count** |
| D11 | Negative-span clamp | phantom (span always ≥ 0) | No |

## Recommended fix order

1. **D10** — change `total = td5_game_get_total_actor_count()` to `total = g_racer_count` (the count of racing vehicles, NOT including traffic). This is the only behavioral divergence with real impact.

2. (Optional) **D1** — bucket wrap. Defer pending impact measurement.

All other D-points are confirmed-faithful or document-only.

## Capture schema for pilot

Per call (one row per `ResolveVehicleContacts` invocation, plus per-pair sub-rows):

**Function-entry keys:** `sim_tick`, `phase` ("entry"/"phase1_done"/"phase2_done")
**Function-entry data:** `racer_count`, `total_actor_count`

**Per-slot Phase 1:** `sim_tick`, `slot`, `world_x_sar8`, `world_z_sar8`, `cardef_radius`, `span`, `bucket`, `prev_head` (= bounds[slot].chain after insert), `aabb_xmin/zmin/xmax/zmax`

**Per-pair Phase 2:** `sim_tick`, `slot_a`, `slot_b`, `dispatch` ("full" or "simple"), `slotA_mode`, `slotA_wcb`, `slotB_mode`, `slotB_wcb`

Schema is ~15 columns; manageable with a single CSV per pool slot.

## Capture scenario

The instruction specifies Moscow span 175 (`--DefaultTrack=4 --StartSpanOffset=175`) for traffic-dense capture, which exposes both racer-traffic interactions and racer-racer.

Alternative: Edinburgh (track=1, slot 0, span 0) — sparse V2V (only 6 racers), but matches existing pool1/pool5 baselines.

Recommendation: **start with Moscow** to exercise the D10 divergence first (port-only traffic V2V events will show up).

## Pair-event indexing

Per the prompt's instruction "key by pair-event index since position drift makes same-tick pairing impossible across port/orig":

Each pair test in Phase 2 contributes a row. Index pairs by `(sim_tick, slot_a, slot_b)` for cross-port pairing. If position drift makes the SAME (slot_a, slot_b) pair miss the same sim_tick, fall back to per-pair sequence number within the function call.

## Blocked by upstream — runtime row-by-row diff cannot be performed (likely)

Like prior pilots (pool1 / pool5 / pool14), the port's AutoRace launch path on Moscow (track=4) with `PlayerIsAI=1` may not bind slot 0 to the correct spawn (Moscow span 175). Until upstream bindings clear:
- Port may produce different actor positions, span_normalized, and racer_count values at sim_tick 1.
- Cross-port pairing of pair-events will diverge from row 1.

**Static-port deliverable:** Even without clean runtime diff, this branch can land the D10 fix and document the audit. Validation via captured-input replay is feasible if the trace harness emits the right keys.

## Reference

- Listing: 0x00409150..0x004092CD — TD5_pool9, 2026-05-14
- Decompilation: same session
- Port: `td5mod/src/td5re/td5_physics.c:3175-3333`
- Caller: RunRaceFrame @ 0x0042B580
- Callees: ResolveVehicleCollisionPair @ 0x00408A60 (pool15), ResolveSimpleActorSeparation @ 0x00408F70

## Why D10 is the only real divergence

Phase 1 (build buckets), Phase 2 (walk pairs), and the dispatch gate are all line-by-line faithful to the listing. The bucket-wrap (D1) is a real difference but only manifests on tracks where `span >> 2 >= 256` (most tracks stay under). The dispatch gate handles vehicle_mode + wcb correctly.

The single algorithmically-significant divergence is the iteration bound. Original iterates `g_racerCount` slots (racers + AI, NOT traffic). Port iterates total (racers + AI + traffic). This causes the port to broadphase-test traffic actors against each other AND against racers, while the original handles traffic-V2V in `UpdateTrafficActorMotion` per-actor.

If the port wants byte-faithful behavior, it must iterate only `g_racer_count` here and let traffic-V2V be handled by its own path (which the port may or may not have ported elsewhere).
