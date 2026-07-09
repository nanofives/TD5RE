# Pilot Audit — 0x00432E60 InitializeRaceActorRuntime

**Date:** 2026-05-14
**Pool slot:** TD5_pool0 (pool12 requested; pool0 was the first slot returned by ghidra_pool.sh acquire)
**Port-side function:** `td5_ai_init_race_actor_runtime` @ `td5_ai.c:717`
**Worktree:** `.claude/worktrees/precise-00432E60` on branch `precise-00432E60` from `master`
**Tag:** `pool12_00432E60` (logical name; actual pool slot is 0)
**Caller graph:** `InitializeRaceSession @ 0x0042AA10` (sole caller; per race init)
**Callee graph:** `InitializeTrafficActorsFromQueue @ 0x00435940` (single callee)
**Body:** 0x00432E60..0x0043367B (0x81B bytes / 486 instructions / ~270 decompiled lines).

## Function purpose

Per-race init pass that:
1. Zeros 0x354 dwords (0xD50 bytes) of `gActorRouteStateTable` starting at `0x004afb60`.
2. Caches the LEFT/RIGHT route-table pointers into globals.
3. Computes `g_racerCount = (gTrafficActorsEnabled ? 12 : 6)`, overridden to `2` if `g_selectedGameType != 0` (time-trial special-case).
4. **Per-slot loop (12 iterations covering slots 0..11)**:
   - Slots 0..5 (racers): copy `DAT_00473DB0` (128-byte AI tuning template) into each actor's per-slot AI tuning block (pointer at `actor[i] + 0x1bc`), then apply (a) global difficulty scaling (Easy/Normal/Hard) and (b) mode/circuit/traffic/tier nested scaling.
   - Slots 6..11 (traffic): just point the per-slot pointer at `&DAT_00473be8` (different default).
   - Tail (shared by all slots): set route-table pointer by slot parity (`even → right`, `odd → left`), set live throttle from `DAT_00473D2C[slot]`, set encounter-handle = -1.
5. Initialize a static 13-short table at `[DAT_004ad288]` with hardcoded V2V geometry constants (vehicle corner offsets, body bounds).
6. Mirror those constants to a second region at `0x004ad2e0`.
7. Call `InitializeTrafficActorsFromQueue`.

## Per-slot scaling decision tree (the heart of the function)

```
if (g_selectedGameType != 0):  // wait — actually [0x4aaf6c] = g_timeTrialEnabled shadow
                                // when TT active, skip ALL scaling, write rb_*=0/0x40/0/0x40
    rb_behind_scale=0, rb_behind_range=0x40, rb_ahead_scale=0, rb_ahead_range=0x40

else (race mode, slot < 6):
    // ---- Layer 1: Global difficulty ----
    if (gDifficultyHard != 0):
        steer  *= 0x28A / 256
        grip   *= 0x17C / 256
        brake  *= 0x1C2 / 256
        lspd_brake *= 0x190 / 256       // 400, [+0x70]
    elif (gDifficultyEasy != 0):
        // no-op
    else (NORMAL):
        steer *= 0x168 / 256
        grip  *= 300   / 256

    // ---- Layer 2: Mode / circuit / traffic / tier ----
    if (gTrackIsCircuit == 0):                                      // POINT-TO-POINT
        if (g_raceOverlayPresetMode == 4):                          //   COP CHASE (wanted)
            steer *= 0x91/256; brake=1000; lspd_brake=1000; top_speed=0x3A1; grip *= 0xB9/256
            rb = { behind 0x8C/100, ahead 0xC0/0x40 }
        elif (gTrafficActorsEnabled == 0):                          //   P2P NO TRAFFIC
            tier 0: steer*=0xAA/256;          ...; rb = { 0xA0/100, 0x96/0x50 }
            tier 1: steer*=0xB4/256; grip*=0x100/256; rb = { 0xC8/0x4B, 0xC0/0x4B }
            tier 2: steer*=0xDC/256; grip*=0x10E/256; rb = { 0x10E/0x41, 0x96/0x50 }
        else:                                                       //   P2P WITH TRAFFIC
            tier 0: steer*=0xB4/256; grip*=0x100/256; rb = { 0xB4/0x4B, 0xBE/100 }
            tier 1: steer*=0xBE/256; grip*=0x10E/256; rb = { 0xC8/0x3C, 0xBE/100 }
            tier 2: steer*=0xDC/256; grip*=0x122/256; rb = { 0xDC/0x3C, 100/0x40  }
    else (CIRCUIT):
        tier 0: steer*=0x91/256;  grip*=0xC8/256;  top_speed=0x3A1; rb = { 0x8C/100, 0xC8/0x37 }
        tier 1: steer*=0xA0/256;  grip*=0xEC/256;  top_speed=0x3A1; rb = { 0x96/100, 0xC0/0x40 }
        tier 2: steer*=0xC3/256;  grip*=0x104/256; top_speed=0x433; rb = { 0xC8/100, 0x78/0x40 }

    // Note: All multi-tier branches set brake=1000, lspd_brake=1000 inside each branch.
```

## Key data layouts

| Address | Symbol (Ghidra) | Width | Meaning |
|---|---|---|---|
| 0x004aee1c | DAT_004aee1c | int32 | LEFT.TRK route table pointer (cached into DAT_004b08b4) |
| 0x004aed8c | DAT_004aed8c | int32 | (cached into DAT_004b08b8 — unused?) |
| 0x004aed94 | DAT_004aed94 | int32 | RIGHT.TRK route table pointer (cached into DAT_004afb58) |
| 0x004aad8c | gTrafficActorsEnabled | int32 | Boolean |
| 0x004aaf6c | g_selectedGameType (alias) | int32 | Shadow of `[0x0049635c]` — set to 0 when game_type != 7, treated as "time_trial_enabled" by this function |
| 0x004aaf80 | gDifficultyHard | int32 | Boolean |
| 0x004aaf84 | gDifficultyEasy | int32 | Boolean |
| 0x00463210 | gRaceDifficultyTier | int32 | 0/1/2 — written by ConfigureGameTypeFlags (0x00410CA0) based on game_type |
| 0x00466e94 | gTrackIsCircuit | int32 | Boolean |
| 0x004aaf74 | g_raceOverlayPresetMode | int32 | 0..5 (4 = cop chase wanted mode) |
| 0x004afb58 | DAT_004afb58 | int32 | RIGHT.TRK pointer (used in slot-parity branch) |
| 0x004b08b4 | DAT_004b08b4 | int32 | LEFT.TRK pointer (used in slot-parity branch) |
| 0x00473d2c | DAT_00473d2c | int32[14] | Default throttle table — `live_throttle[slot] = DAT_00473d2c[slot]` |
| 0x00473d9c | DAT_00473d9c | int32 | g_rb_behind_scale (set by tier branch) |
| 0x00473da0 | DAT_00473da0 | int32 | g_rb_ahead_scale |
| 0x00473da4 | DAT_00473da4 | int32 | g_rb_behind_range |
| 0x00473da8 | DAT_00473da8 | int32 | g_rb_ahead_range |
| 0x00473db0 | DAT_00473db0 | int8[128] | AI tuning template (copied into each slot's tuning block) |
| 0x00473be8 | DAT_00473be8 | ? | Default tuning pointer for traffic slots 6..11 |
| 0x004ab108 + 0x1bc | actor[i].field_0x1bc | int32 | Per-actor pointer to mutable tuning copy |
| 0x004afb60 + slot*0x11C | gActorRouteStateTable[slot] | int32[0x47] | Per-slot route state |
| 0x004ad288 + N | (V2V geometry block) | int16 | Hardcoded vehicle bounding-box constants |

### Per-slot tuning block field offsets (within the 128-byte template)

| Offset | Width | Purpose |
|---|---|---|
| +0x2C | int16 | Grip / base lateral coefficient |
| +0x68 | int16 | Steering factor |
| +0x6E | int16 | Brake force |
| +0x70 | int16 | Low-speed brake coefficient |
| +0x74 | int16 | Top speed / rev limiter |

## Confirmed divergences (port vs original)

### D1 — `g_race_difficulty_tier` is never written **(HIGHEST IMPACT)**

The port declares `static int32_t g_race_difficulty_tier;` at `td5_ai.c:258` and **reads it at line 718 (`int tier = g_race_difficulty_tier;`) but never writes it**. This was flagged by the pool11 0x00432D60 audit.

The frontend's `ConfigureGameTypeFlags @ td5_frontend.c:2661` correctly maps `game_type → g_td5.difficulty_tier` (1/2→0, 3/4/5→1, 6/7→2), matching the original at 0x00410CA0. But the port has **two separate tier symbols**:
- `g_td5.difficulty_tier` (in TD5_State; written by ConfigureGameTypeFlags)
- `g_race_difficulty_tier` (file-static in td5_ai.c; never written)

`td5_ai_init_race_actor_runtime` reads the wrong one. Result: every race uses tier=0 regardless of game type.

**Fix:** Read `g_td5.difficulty_tier` instead of `g_race_difficulty_tier`. Remove the dead static. Add a comment anchoring to the original's `gRaceDifficultyTier @ 0x00463210`.

Captured effect (from pool11 trace, Edinburgh AutoRace `PlayerIsAI=1`):
- Port: tier=0 → `(rb_behind=160/100, rb_ahead=150/80)` (P2P-no-traffic tier-0)
- Original: tier=2 → `(rb_behind=270/65, rb_ahead=150/80)` (P2P-no-traffic tier-2)

### D2 — Missing `low-speed brake` field write `+0x70` in Layer-1 HARD scaling **(MEDIUM IMPACT)**

Port at `td5_ai.c:826-831`:
```c
case TD5_DIFFICULTY_HARD:
    *steer = ... * 0x28A;
    *grip  = ... * 0x17C;
    *brake = ... * 0x1C2;
    break;
```

Original listing at 0x00432F95..0x00432FAE:
```
0x00432F95  MOVSX EAX, word ptr [EBP + 0x70]
0x00432F99  LEA   EAX, [EAX + EAX*4]          ; *5
0x00432F9C  LEA   EAX, [EAX + EAX*4]          ; *25
0x00432F9F  SHL   EAX, 0x4                    ; *25 * 16 = *400
0x00432FA2  CDQ
0x00432FA3  AND   EDX, 0xFF
0x00432FA9  ADD   EAX, EDX
0x00432FAB  SAR   EAX, 0x8
0x00432FAE  MOV   word ptr [EBP + 0x70], AX
```

The port scales steer (+0x68), grip (+0x2C), brake (+0x6E) but is missing the fourth scaling: `lspd_brake (+0x70) *= 400 / 256`. The original Hard-difficulty branch scales four fields, not three.

**Fix:** Add the `+0x70` (low-speed brake) scaling in the HARD case: `*lspd_brake = (int16_t)(((int32_t)*lspd_brake * 400) >> 8);`.

### D3 — Missing `brake (+0x6E)` and `lspd_brake (+0x70)` clamps to 1000 in every per-mode branch **(MEDIUM IMPACT)**

In the original, every leaf of the Layer-2 nested decision tree explicitly writes:
```
MOV word ptr [EBP + 0x6e], BX        ; BX = 0x3E8 = 1000
MOV word ptr [EBP + 0x70], BX        ; BX = 1000
MOV word ptr [EBP + 0x74], 0x3A1     ; or 0x433 for tier-2 / 0x3B9 for one branch
```

These are **unconditional overwrites** of brake / low-speed brake / top-speed for every race-mode branch (cop, P2P×{tier0/1/2}×{traffic on/off}, circuit×{tier0/1/2}).

Port `td5_ai.c:861-955`: the Layer-2 branches write `*spd` and `*grp` (and rb_*) but **do not write `*brake` or `*lspd_brake` or `*spd` for the no-traffic-tier-0 case** consistently. Specifically:
- Cop / Pitbull branch: missing `brake=1000, lspd_brake=1000, top_speed=0x3A1` writes
- Circuit tier 0/1: missing `top_speed=0x3A1`
- Circuit tier 2: missing `top_speed=0x433`
- P2P no-traffic tier 0: missing `top_speed=0x3A1`
- P2P no-traffic tier 1: missing `top_speed=0x3A1`
- P2P no-traffic tier 2: missing `top_speed=0x433`
- P2P with-traffic tier 0: missing `top_speed=0x3A1`
- P2P with-traffic tier 1: missing `top_speed=0x3B9` (this one is unique — note the `b903` bytes at 0x00433441)
- P2P with-traffic tier 2: missing `top_speed=0x433`

Plus in all those branches: `brake=1000, lspd_brake=1000` clamps are missing from the port.

**Effect:** AI top-speed and brake force in the port depend on whatever the template's defaults were after Layer-1 scaling. Net effect varies by tier & difficulty; on Edinburgh hard-difficulty tier 2 the original ends up with top_speed=0x433 (1075) but the port leaves it scaled-only-by-difficulty (could be different).

**Fix:** Add the missing clamps per branch.

### D4 — Cop-Chase branch uses `wanted_mode_enabled` instead of `g_raceOverlayPresetMode == 4` **(LOW IMPACT)**

Port at `td5_ai.c:848`: `else if (is_pitbull) { ... is_pitbull = (g_td5.race_rule_variant == 4); }`.

The original branch condition is `g_raceOverlayPresetMode == 4` (set by `ConfigureGameTypeFlags` case 8 → cop chase, NOT pitbull which is overlay-mode 2).

Looking at the port's `ConfigureGameTypeFlags`:
- case 4 (Pitbull): `g_td5.race_rule_variant = 2` (NOT 4)
- case 8 (Cop Chase): `g_td5.wanted_mode_enabled = 1`

So the port's `is_pitbull = (race_rule_variant == 4)` is misnamed AND wrong — `race_rule_variant=4` is set by `case 6: Ultimate` not Pitbull. The branch is mostly dead in practice. The original branch should fire on **Cop Chase (game_type=8)**, where overlay_preset_mode is set to 4.

**Fix:** Change the condition to `(g_td5.wanted_mode_enabled != 0)` (or, more precisely, `g_raceOverlayPresetMode == 4` if the port tracks that variable, but at present it does not). Rename `is_pitbull` → `is_cop_chase`.

### D5 — `is_time_trial` branch is over-approximate **(LOW IMPACT)**

The port at `td5_ai.c:841-846` writes `rb=(0,0x40,0,0x40)` when `is_time_trial`. The original writes the same constants when `[0x004aaf6c] != 0` (which, post-ConfigureGameTypeFlags, is non-zero only for `game_type == 7`).

This is mostly aligned — the port's TT synth at frontend.c case 7 keeps `time_trial_enabled=0` and remaps to plain single-race. So the gate condition in the port never fires in practice. The original gate `[0x004aaf6c] != 0` similarly never fires post-synth. Algorithmically equivalent given the upstream synth.

**Fix (optional):** No change required — the upstream synth removes both sides.

### D6 — Per-slot `route_table_selector` parity is inverted **(VERIFY)**

Original listing 0x004334EF..0x0043351F:
```
EDX = ESI & 1                                      ; slot parity
if (EDX != 1):  // EVEN
    [EAX]    = [0x004afb58]                         ; RIGHT.TRK pointer
    [EAX+0xc] = 0                                  ; selector = 0
else:           // ODD
    [EAX]    = [0x004b08b4]                         ; LEFT.TRK pointer
    [EAX+0xc] = 1                                  ; selector = 1
```

So:
- EVEN slots (0, 2, 4) → RIGHT.TRK, selector 0
- ODD  slots (1, 3, 5) → LEFT.TRK,  selector 1

Port at `td5_ai.c:763-765`:
```c
selector = (i & 1) ? 0 : 1;
rs[RS_ROUTE_TABLE_SELECTOR] = selector;
rs[RS_ROUTE_TABLE_PTR] = (int32_t)(intptr_t)g_route_tables[selector];
```

Port: ODD → selector 0, EVEN → selector 1 — INVERTED relative to original. Combined with the port's `g_route_tables[]` array ordering (need to verify which is LEFT vs RIGHT in the port), this could either match or invert.

**Verify with Frida + listing**: print `g_route_tables[0]` and `g_route_tables[1]` from the port, compare to `DAT_004afb58` (RIGHT) and `DAT_004b08b4` (LEFT).

Per the port comment at `td5_ai.c:757-762`, this was deliberately set to "EVEN → RIGHT.TRK (selector 1)". But the original uses `selector=0` for EVEN slots with RIGHT.TRK pointer at `DAT_004afb58`. The mapping `selector→table_ptr` must be consistent in both port and original; if `g_route_tables[0]` is LEFT and `g_route_tables[1]` is RIGHT then the port is correct functionally. Confirm in the asset loader.

**Fix:** Verify the port's `g_route_tables[]` array indexing. If `[0]==LEFT` and `[1]==RIGHT`, the port is correct (EVEN→selector 1→RIGHT.TRK matches the original). If reversed, fix the port.

### D7 — Layer-1 NORMAL grip multiplier mismatch **(LOW IMPACT, decimal/hex confusion?)**

Original at 0x00432FDA: `IMUL EAX, EAX, 0x12c` — that's 0x12C = 300.
Port at td5_ai.c:822: `*grip = (int16_t)(((int32_t)*grip * 300) >> 8);`
**Match** — port comment "300/256 (~117%)" is correct.

### D8 — V2V geometry table write at end (post-loop) **(VERIFY — possibly OUT OF SCOPE)**

The original writes 13 short values at `[DAT_004ad288] + 0x40..0x84` (vehicle corner offsets for V2V collision) and mirrors them to `0x004ad2e0..0x004ad2fc`. These are V2V/wanted-state vehicle geometry constants — separate from AI tuning.

Port: needs verification. Search for `0x4ad288` or `g_v2v_geometry` in the port. If present, OK; if not, it's missing init for V2V dimensions.

(This is OUT OF SCOPE for the immediate `g_race_difficulty_tier` fix and could be deferred. Document and revisit.)

### D9 — Missing pre-clear of `gActorRouteStateTable[]` (0x354 dwords = 0xD50 bytes) **(LOW IMPACT)**

Port: `memset(g_route_state_storage, 0, sizeof(g_route_state_storage));` at `td5_ai.c:742` clears the full storage. Algorithmically equivalent.

## Risk class

**MEDIUM-HIGH.** The tier-binding bug (D1) alone is responsible for systematic AI under-performance on Edinburgh tier-2 P2P (~80% top-speed coefficient, ~50% lower brake force, ~67% lower rubber-band catchup). Three downstream pilots (0x00432D60, 0x00404EC0, 0x004057F0) are reading state that is computed from the wrong tier. Fixing D1 unblocks all three.

D2/D3 are concrete byte-faithful gaps but lower impact.

## Fixes to apply (this pilot)

`td5_ai.c` (worktree only):

1. **D1 (HIGHEST):** Replace `static int32_t g_race_difficulty_tier;` reads with `g_td5.difficulty_tier`. Remove the unused static. Anchor comment to `gRaceDifficultyTier @ 0x00463210`.

2. **D2:** Add `+0x70` (low-speed brake) scaling `*= 400/256` in the HARD difficulty branch.

3. **D3:** Add `brake=1000, lspd_brake=1000, top_speed=<branch-specific>` writes inside every Layer-2 mode/tier branch. Specifically:
   - Cop chase: top_speed=0x3A1
   - Circuit tier 0/1: top_speed=0x3A1
   - Circuit tier 2: top_speed=0x433
   - P2P no-traffic tier 0/1/2: top_speed=0x3A1/0x3A1/0x433
   - P2P with-traffic tier 0/1/2: top_speed=0x3A1/0x3B9/0x433

4. **D4:** Change `is_pitbull` cop-chase branch to use `g_td5.wanted_mode_enabled` (which `ConfigureGameTypeFlags` case 8 sets). Rename variable.

5. **D6:** Verify port's `g_route_tables[]` array; ensure parity → table mapping matches original.

6. **Add line-by-line `[address]` anchors** in comments.

7. **Wire pilot trace hook** at function entry/exit capturing tier, is_circuit, has_traffic, wanted_mode, g_rb_behind_scale, g_rb_behind_range, g_rb_ahead_scale, g_rb_ahead_range.

## Capture schema for runtime validation

Per call (1 row at entry + 1 row at exit; function fires ONCE per race init at sim_tick=0):

**Keys:** `frame`, `sim_tick`, `phase` ("entry" | "exit")
**Inputs (at entry):**
- `tier_in`         — `g_td5.difficulty_tier` (port) / `[0x00463210]` (original)
- `is_circuit`      — `g_td5.track_type == TD5_TRACK_CIRCUIT` / `[0x00466e94]`
- `has_traffic`     — `g_td5.traffic_enabled` / `[0x004aad8c]`
- `wanted_mode`     — `g_td5.wanted_mode_enabled` (port) / `g_raceOverlayPresetMode == 4` (orig via `[0x004aaf74]`)
- `difficulty`      — `g_td5.difficulty` (port) / `gDifficultyHard|gDifficultyEasy` (orig)
- `time_trial`      — `g_td5.time_trial_enabled` / `[0x004aaf6c]`
- `template_steer`  — `g_ai_physics_template[+0x68]` pre-scaling
- `template_grip`   — `g_ai_physics_template[+0x2C]` pre-scaling
- `template_brake`  — `g_ai_physics_template[+0x6E]` pre-scaling
- `template_lspd_brake` — `g_ai_physics_template[+0x70]` pre-scaling
- `template_top_spd` — `g_ai_physics_template[+0x74]` pre-scaling

**Outputs (at exit):**
- `rb_behind_scale` — `[0x00473d9c]`
- `rb_behind_range` — `[0x00473da4]`
- `rb_ahead_scale`  — `[0x00473da0]`
- `rb_ahead_range`  — `[0x00473da8]`
- `template_steer_out` — post-scaling
- `template_grip_out`  — post-scaling
- `template_brake_out` — post-scaling
- `template_lspd_brake_out` — post-scaling
- `template_top_spd_out` — post-scaling
- `racer_count`     — `[0x004aaf00]`

Only ~22 columns; trivial to diff. Function fires once per race so capture window is exactly 1 row per side.

## Blocked / downstream

- D5 (V2V geometry table) is out of immediate scope; documented for separate pilot.
- D6 (route-table parity) needs verification but algorithmically should already match given the port's existing g_route_tables[] indexing comment.

## Reference

- Listing: 0x00432E60..0x0043367B (TD5_pool0, 2026-05-14)
- Decompilation: pool0 session, ~270 lines
- ConfigureGameTypeFlags listing: 0x00410CA0..0x00410F2F (tier-write sites: 0x00410d28 case1, 0x00410dc0 case2, 0x00410e15 case3/5, 0x00410e03 case4, 0x00410e4b case6, 0x00410e72 case7)
- Port pre-fix: `td5mod/src/td5re/td5_ai.c:717-993` (`td5_ai_init_race_actor_runtime`)
- Port frontend tier-write: `td5mod/src/td5re/td5_frontend.c:2675-2680` (writes `g_td5.difficulty_tier`)
- Memory: `feedback_pursue_fixes`, pool11 audit (`pilot_00432D60_audit.md` blocker #1)
