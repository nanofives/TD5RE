---
batch: 30
area: particle_pool
tier: T6
target_todos: [todo-smoke-render-broken-2026-05-19]
ghidra_session: TD5_pool3
analyzed_addresses: 0x004296b0, 0x00429c00, 0x00429d00, 0x00429f00, 0x0042a440, 0x004295e0
agent: tier1-a-t6
date: 2026-05-22
---

# Globals enumeration — particle pool state (T6)

## Summary

- Functions analyzed: InitializeRaceParticleSystem, SpawnVehicleSmokeSprite, SpawnVehicleSmokeVariant, SpawnVehicleSmokePuffFromHardpoint, SpawnVehicleSmokePuffAtPoint, SpawnAmbientParticleStreak, UpdateRaceParticleEffects, ProjectRaceParticlesToView
- Unnamed DAT_* targeted: 16 fields in W1-E cluster C.4 + a few neighbors
- Already-named neighbors noted: g_raceParticlePoolBase
- Proposals — high: 11
- Proposals — medium: 4
- Proposals — comment-only: 1

## Methodology

These addresses ALL appear as `[ESI + 0x4a31XX]` in the spawn functions, where ESI is the per-particle base pointer. The literal addresses are NOT the data itself; they are **compile-time field offsets baked as absolute displacements** in MSVC-generated code. This means the *struct* lives wherever ESI points (per particle), and 0x004a31XX..0x004a31cf gives the **field offset signature** of a single particle. Each spawn function writes the SAME set of fields with role-specific values, telling us what each offset means.

Cluster signature (per-particle struct, ESI-relative):
- +0x001 (DAT_004a3171): byte type/category — written once per spawn from spawn-fn-specific value
- +0x002 (DAT_004a3172): byte life/state — written once per spawn
- +0x004 (DAT_004a3174): word fixed-point Y_init (0x9000/0x7000/0x4000) — initial Y for each smoke variant
- +0x006 (DAT_004a3176): word param X (AX from caller — heading/velocity X)
- +0x008 (DAT_004a3178): word fixed-point Y_offset (0x2080/0x26c0) — secondary Y param
- +0x00a (DAT_004a317a): word param Z (AX from caller)
- +0x00c (DAT_004a317c): dword EAX from caller — owner actor pointer
- +0x010 (DAT_004a3180): dword fixed-point velocity (0x2000/0x1800/0x600 per variant)
- +0x014 (DAT_004a3184): dword EAX/ECX from caller — accel/scale
- +0x01f (DAT_004a318f): byte life=0x80 — initial-alive byte (constant 0x80 across all spawn fns)
- +0x020 (DAT_004a3190): dword world_x — written from EDX/ECX
- +0x024 (DAT_004a3194): dword world_y — written from ECX/EDX
- +0x028 (DAT_004a3198): dword world_z — written from EDX/EAX
- +0x038 (DAT_004a31a8): function-ptr `0x004297d0` — `update_fn` (SAME 3 callers write SAME ptr)
- +0x03c (DAT_004a31ac): function-ptr `0x00429950` — `render_fn` (SAME 3 callers write SAME ptr)
- +0x05f (DAT_004a31cf): byte tail-flag (sentinel/end-marker; 0x80 from InitializeRaceParticleSystem)

## Proposals (globals)

Note: these all live at addresses that are **field-offset displacements**, not real storage. We propose renaming them to clarify struct semantics so decompiles read as `particle->type`, `particle->update_fn` etc. The renames don't change physical layout; they restyle decomp output.

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004a3171 | u8 (offset) | `g_particle_offset_type` | high | `[ESI + 0x4a3171] = AL` (caller-specific value 1/2/3) across 3 spawn fns (9 refs) | `(none)` |
| 0x004a3172 | u8 (offset) | `g_particle_offset_state` | high | `[ESI + 0x4a3172] = AL` across same callers (9 refs) | `(none)` |
| 0x004a3174 | u16 (offset) | `g_particle_offset_initY` | high | Written w/ constants 0x9000/0x7000/0x4000 in 3 variants (9 refs) | `(none)` |
| 0x004a3176 | u16 (offset) | `g_particle_offset_paramX` | high | `[ESI+offset] = AX` (9 refs) | `(none)` |
| 0x004a3178 | u16 (offset) | `g_particle_offset_initYoffset` | high | Written w/ 0x2080/0x26c0 (9 refs) | `(none)` |
| 0x004a317a | u16 (offset) | `g_particle_offset_paramZ` | high | `[ESI+offset] = AX` (5 refs) | `(none)` |
| 0x004a317c | u32 (offset) | `g_particle_offset_ownerPtr` | medium | `[ESI+offset] = EAX` (6 refs) — caller passes actor ptr | `(none)` |
| 0x004a3180 | u32 (offset) | `g_particle_offset_velocity` | high | Constants 0x2000/0x1800/0x600 (5 refs) | `(none)` |
| 0x004a3184 | u32 (offset) | `g_particle_offset_accel` | medium | `[ESI+offset] = EAX/ECX` (7 refs) | `(none)` |
| 0x004a318f | u8 (offset) | `g_particle_offset_aliveByte` | high | Always written `0x80` across all spawn variants (26 refs incl init+cmp 0x0) | `(none)` |
| 0x004a3190 | u32 (offset) | `g_particle_offset_worldX` | high | `[ESI+offset]=EDX/ECX`; ProjectRaceParticlesToView FILD `[ESI+1]` confirms +1 from this base = X-component start (8 refs) | `(none)` |
| 0x004a3194 | u32 (offset) | `g_particle_offset_worldY` | high | Same pattern; ProjectRaceParticlesToView FILD `[ESI+5]` (9 refs) | `(none)` |
| 0x004a3198 | u32 (offset) | `g_particle_offset_worldZ` | high | Same pattern; ProjectRaceParticlesToView FILD `[ESI+9]` (8 refs) | `(none)` |
| 0x004a31a8 | u32 (offset) | `g_particle_offset_updateFn` | high | All 3 spawn fns write `0x004297d0` (8 refs) — function pointer | `(none)` |
| 0x004a31ac | u32 (offset) | `g_particle_offset_renderFn` | high | All 3 spawn fns write `0x00429950` (6 refs) — function pointer | `(none)` |
| 0x004a31cf | u8 (offset) | `g_particle_offset_tailSentinel` | medium | CMP `[EAX],0x0` walk-terminator + Init writes 0x80 (9 refs) | `(none)` |
| 0x004a6371 | u8 (offset) | `g_particle_offset_uvCellIndex` | high | `[ESI + 0x4a6370 + EAX]` in SpawnVehicleSmokeSprite, `[EDI + 0x4a6370]` in InitializeRaceParticleSystem (13 refs) — UV-cell-index byte at +1 of a 2-byte particle sprite field at 0x4a6370 | `(none)` |
| 0x004aabbc | u32 | `g_particleSpawnPolicyMode` | medium | InitializeRaceParticleSystem sole writer/reader (6 refs) | `(none)` |

## Key discoveries

- The base of the per-particle struct is at +0 of ESI. Looking at offsets, the struct size is at least 0x60 (96 bytes) per particle. Wave 2 should type this as `RaceParticle` struct using the offsets above.
- `g_particle_offset_updateFn = 0x004297d0` and `g_particle_offset_renderFn = 0x00429950` are **function pointers** baked in at spawn time. This is a virtual dispatch table-per-instance pattern (no struct vtable). All 3 spawn-fn variants write the SAME pair of fn ptrs → all variants of smoke share the same update+render logic; the differentiation lives in the **init constants** (initY, velocity, paramX/Z).
- 0x004a318f is a **liveness byte** (always written 0x80 on spawn; CMP==0 in iteration → "alive while !=0"). NOT a phase counter. This contradicts any port assumption that smoke fades by counter.
- The smoke render audit (memory `reference_smoke_alpha_audit_correction_2026-05-20`) confirms `0xff` write at spawn and no fade — `g_particle_offset_aliveByte` corroborates: alive=0x80 not alpha; alpha hardcoded elsewhere.

## Out-of-scope finds

| address | brief note | probable area |
|---|---|---|
| 0x004a312c | u32 | InitializeRaceParticleSystem only | particle init |
| 0x004a2c94 | u32 | TrackSelectionScreenStateMachine | frontend |
| 0x004a2cc8 | u32 | (3 refs only) | |

## TODO impact

- **todo-smoke-render-broken-2026-05-19**: Reinforces the existing closure analysis (alpha not faded; smoke renders correctly per spawn). Names cement particle struct semantics for future port-side cleanup. Does NOT introduce a new fix but locks in the diagnosis.
