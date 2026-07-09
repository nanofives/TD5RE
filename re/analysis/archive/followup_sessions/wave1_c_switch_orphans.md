# Wave 1 Agent C — Switch-Table Orphan Recovery

**Date:** 2026-05-22
**Scope:** Item 7. Investigate the 3 functions Ghidra named `caseD_*` to determine if they are reachable (live switch targets) or dead code.
**Method:** Read-only Ghidra (TD5_pool13). Cross-reference TO each address, locate `JMP DWORD PTR [TABLE + idx*4]` dispatch site, read table bytes, verify each orphan address is a live table entry.

## Summary

**All three `caseD_*` are LIVE switch targets.** They are reachable case bodies in two state-machine functions. Ghidra named them `caseD_<index>` because it identified the jump-table dispatch but did not promote them to nested case labels in the parent's body listing. The decompiler nevertheless folds them back into the parent's `switch` statement correctly.

| Orphan         | Addr        | Size | Parent function                       | Switch base   | Case idx | Verdict |
|----------------|-------------|------|---------------------------------------|---------------|----------|---------|
| `caseD_7`      | 0x004173B1  |  284 | `RaceTypeCategoryMenuStateMachine` @ 0x004168B0 | 0x00417CD4    | 7        | LIVE    |
| `caseD_a`      | 0x00417700  | 1490 | `RaceTypeCategoryMenuStateMachine` @ 0x004168B0 | 0x00417CD4    | 10       | LIVE    |
| `caseD_6`      | 0x0041808D  |   37 | `ScreenExtrasGallery` @ 0x00417D50              | 0x00418390    | 6        | LIVE    |

## Evidence

### Dispatch 1 — `RaceTypeCategoryMenuStateMachine` (covers `caseD_7`, `caseD_a`)

Dispatch site at 0x004168E2:

```
004168D4  SUB ESI,0x9F             ; (canvas-half-height bias, unrelated)
004168D9  MOV EBP,0x14             ; upper bound EBP = 20
004168DA  CMP EAX,EBP
004168DC  JA  0x00417CCE           ; default
004168E2  JMP dword ptr [EAX*4 + 0x417CD4]   ; switchD
```

So 21 cases (0..20). Table at 0x00417CD4, 84 bytes:

```
case 0  → 0x004168E9
case 1  → 0x00416A81
case 2  → 0x00416BEB
case 3  → 0x00416C4B
case 4  → 0x00416D80
case 5  → 0x00416F16
case 6  → 0x0041707A
case 7  → 0x004173B1  ← caseD_7
case 8  → 0x004174CE
case 9  → 0x004175BC
case 10 → 0x00417700  ← caseD_a
case 11 → 0x00417918
case 12 → 0x00417B9A
case 13..19 → 0x00417CCE  (default/no-op tail)
case 20 → 0x00417CBA
```

The DATA xrefs found by Ghidra confirm:
- `caseD_7` is referenced from 0x00417CF0 = 0x00417CD4 + 7×4 (table[7])
- `caseD_a` is referenced from 0x00417CFC = 0x00417CD4 + 10×4 (table[10])

Both also carry a `COMPUTED_JUMP` reference back from the dispatch instruction at 0x004168E2.

`decomp_function(0x004168B0)` produces a single `switch(g_frontendInnerState)` containing `case 7:` (sprite-sweep transition; size matches the 284-byte body of `caseD_7`) and `case 10:` (final fade-out / `SetFrontendScreen(g_returnToScreenIndex)` exit; size matches the 1490-byte body of `caseD_a`). These are the FSM states that transition between the Single-Race / Cup / Championship sub-menus.

### Dispatch 2 — `ScreenExtrasGallery` (covers `caseD_6`)

Dispatch site at 0x00417D65:

```
00417D50  MOV ECX,dword ptr [0x00495204]   ; FSM state variable
00417D56  SUB ESP,0x10
00417D59  CMP ECX,0x7                       ; upper bound = 7
00417D5C  PUSH EBX/ESI/EDI
00417D5F  JA  0x00418386                    ; default
00417D65  JMP dword ptr [ECX*4 + 0x418390]  ; switchD
```

8 cases (0..7). Table at 0x00418390, 32 bytes:

```
case 0 → 0x00417D6C
case 1 → 0x00417DAC
case 2 → 0x0041807F
case 3 → 0x0041807F
case 4 → 0x0041807F
case 5 → 0x0041807F
case 6 → 0x0041808D  ← caseD_6
case 7 → 0x004180B3
```

`caseD_6` DATA xref from 0x004183A8 = 0x00418390 + 6×4 (table[6]), plus the COMPUTED_JUMP back from 0x00417D65. Body is only 37 bytes — short tail state of the Extras gallery FSM.

## L3 reclassification

The confidence-map entries for these three addresses currently record "zero callers, possibly dead". They should be reclassified as **live, indirect entry via in-function jump table**. They are not orphans; they are inline FSM case bodies that the Ghidra namer split out because the parent function had not yet been given a proper body extent at the time those case labels were emitted.

Concretely: no source-port impact (their parents are already ported as switch statements covering all indices). The reclassification is bookkeeping for the L3 manifest only:

- `0x004173B1 caseD_7`  → folds into `RaceTypeCategoryMenuStateMachine` case 7
- `0x00417700 caseD_a`  → folds into `RaceTypeCategoryMenuStateMachine` case 10
- `0x0041808D caseD_6`  → folds into `ScreenExtrasGallery` case 6

No standalone L3 entry warranted.

## Notes / caveats

- Read-only verification only — no Ghidra writes. The decompiler already integrates these bodies into the parent functions when invoked, so no listing changes are required for analysis purposes.
- Cleanup: pool slot will be released via `bash scripts/ghidra_pool.sh cleanup` after this report.
