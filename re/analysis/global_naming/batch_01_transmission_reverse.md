---
batch: 01
area: transmission / reverse / brake-latch
tier: T1
target_todos: [todo_default_transmission_auto_2026-05-19, todo_reverse_not_triggered_2026-05-19]
ghidra_session: f2d5953e06534ac28145ef57d53bcecd
analyzed_addresses: 0x00402E60, 0x0042EF10, 0x0042F010, 0x0042F030, 0x004032A0, 0x004036B0, 0x0042B580
agent: Haiku 4.5
date: 2026-05-20
---

# Globals enumeration — transmission / reverse latch

## Summary

- Functions analyzed: 7
- Unnamed DAT_* globals encountered: 6 (after de-dup)
- Already-named globals encountered: 2 (g_playerControlBits, g_gearTorqueTable, gActorSpecialEncounterActive)
- Proposals — high confidence: 4
- Proposals — medium confidence: 2
- Proposals — comment-only (low confidence): 0

## Methodology

Entry points were the three pre-seeded functions (UpdatePlayerVehicleControlState @ 0x00402E60, UpdateAutomaticGearSelection @ 0x0042EF10, ApplyReverseGearThrottleSign @ 0x0042F010, ComputeDriveTorqueFromGearCurve @ 0x0042F030) plus the brake/reverse latch region at 0x004032A0. Walked outbound callees to UpdateRaceFrame @ 0x0042B580, and inbound callers from UpdatePlayerSteeringWeightBalance @ 0x004036B0. Gate was: any global written or read during per-tick actor control state update, brake-to-reverse latch transition, or automatic gearbox dispatch. Inclusion criterion: written on every frame or conditionally on latch/transmission events, affecting gear/reverse/braking semantics.

## Proposals

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x0046317c | u16[6] | `g_playerSteeringWeightTarget` | high | Written by UpdatePlayerSteeringWeightBalance @ 0x004036de/0x00403708. Read at 0x00403335 (signed extend into steering interpolation). Per-slot steering balance for two-player mode. Clearly per-slot baseline, indexed by slotIndex×2. | `(none)` — not yet ported |
| 0x0046317e | u16[6] | `g_playerSteeringWeightOpposite` | high | Written by UpdatePlayerSteeringWeightBalance @ 0x004036e4. Complement steering weight (0x100 - target). Used in two-player steering balancing. | `(none)` — not yet ported |
| 0x0048301c | u32 | `g_maxSteeringWeightBalance` | high | Read at 0x004036c4 and 0x004036ee in UpdatePlayerSteeringWeightBalance. Written at 0x004155ea in ScreenMainMenuAnd1PRaceFlow (race init). Caps the steering balance delta. | `(none)` — likely INI-driven config |
| 0x00483014 | u8[6] | `g_cheatRemoteBrakingApplied` | high | Reset to 0 @ 0x00402e32 when input bit 0x200000 NOT set; set to 1 @ 0x0042b96b after velocity-doubling cheat applied. Per-slot flag. Checked at 0x0042b96d. | `(none)` — cheat state, not ported |
| 0x00483018 | u16 | `g_controlAccumulatorResetMarker` | medium | Written @ 0x00402e37 in ResetPlayerVehicleControlAccumulators (alongside DAT_00483014). No visible read refs; likely a sentinel or alignment. | `(none)` — initialization only |
| 0x004ab473 | u8 | `g_playerGearIndex` | medium | Referenced in comments and decompilation as the per-actor gear byte at +0x36b. Already symbolically accessed in port but no global label assigned in orig binary. Confirm struct layout. | `td5_physics.c` — already has field naming |

## Key discoveries

- **Input-driven auto_gearbox flag**: Actor field +0x378 is recomputed per-tick from input bit 28 of `g_playerControlBits` at line 0x00402e63: `field_0x378 = ~(bit28) & 1`, NOT seeded from cardef. This inverts the TODO assumption (cardef-seeding). The per-tick recompute means the latch gate is highly responsive but also means if input bit 28 is stuck in wrong state, reverse won't trigger. This is likely the root cause of both reported bugs: if bit 28 mapping in port is wrong (or not implemented), auto_gearbox reads as 0 every frame, blocking the brake→reverse gate at 0x00403369 (`auto_gearbox != 0 && ...`).
  
- **Brake/reverse state machine gated by +0x378**: The control-state machine at ~0x00403340-0x004036a0 (part of UpdatePlayerVehicleControlState) writes reverse-latch bytes (+0x36d/+0x36e/+0x36f) only when the `auto_gearbox` gate is true. If gate is stuck false, the state machine never transitions to reverse mode, and throttle stays positive → drive-torque path runs instead of reverse-torque.

- **Steering weight balance is purely visual/input**: The two-player steering weight globals (0x0046317c/7e) do NOT affect physics; they gate special-encounter steering interpolation only (via bit 0x200 write at 0x00403708 and read at 0x00403335). They have zero impact on the transmission bugs.

- **Cheat-remote-braking flag is non-transmission**: DAT_00483014 is a cheat flag for velocity doubling, reset per-frame if cheat bit not set. Not related to the brake-to-reverse latch.

## Out-of-scope finds

| address | brief note | probable area |
|---|---|---|
| 0x004ab473 | Per-actor gear index byte at offset +0x36b | T1 — transmission data (already informally named in port as field_0x36b) |
| 0x00482fd0 | Per-player control buffer state indexed by slotIndex×4 | T1 — input/control (appears to track debounce or persistent control flags) |
| 0x00466014 | Read at 0x004155e4 in race-init path | T1 — race configuration (unknown purpose; needs further analysis) |
| gActorSpecialEncounterActive | Already named in decompilation; used at 0x00403369 as gate | T1 — special encounter system (orthogonal to transmission, not enumerated here) |

## TODO impact

- **todo_default_transmission_auto_2026-05-19**: **Root cause identified, NOT closed by naming alone.** The bug is NOT missing cardef-seed data (cardef is never the source). The +0x378 flag is recomputed per-tick from input bit 28. If the port's input layer does not correctly map bit 28, or if it defaults to 0, the flag stays 0 → automatic transmission never engages. **Recommend**: Verify bit 28 mapping in `td5_input.c` input-polling code. If missing, add it. If inverted, fix polarity. Test with explicit `auto_gearbox = 1` force at race-start if mapping is uncertain. The cardef byte for auto-gearbox is likely unused in the original code path.

- **todo_reverse_not_triggered_2026-05-19**: **Closes together with above.** The reverse gate requires `auto_gearbox != 0` (field +0x378). If input bit 28 is wrong/stuck, this gate never opens, and the brake-to-reverse latch never transitions. Once input bit 28 is correctly mapped and driving auto_gearbox, re-test reverse via brake-hold at 0 speed; the TODO should close. **No additional physics or latch-logic changes needed** if input layer is fixed.

