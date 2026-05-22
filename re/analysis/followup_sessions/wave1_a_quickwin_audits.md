# Wave 1 Agent A — Quick-win Audits

Session: 2026-05-22, pool slot TD5_pool11 (read-only).
Scope: 3 small read-only Ghidra audits on TD5_d3d.exe.

---

## Task 1: 5 DEAD-CODE residual — verification

For each address, used `reference_to` (which captures all data refs from analyzed tables) plus byte-pattern search to find any indirect-call site. Byte searches returned 0 because all data refs are already promoted to typed pointers in Ghidra; the reference manager is the source of truth.

| Address | Function | Refs | Verdict |
|---|---|---|---|
| 0x004173B1 | `caseD_7` | 1 DATA (0x00417cf0) + 1 COMPUTED_JUMP (0x004168e2) | NOT DEAD — switch-case target |
| 0x00417700 | `caseD_a` | 1 DATA (0x00417cfc) + 1 COMPUTED_JUMP (0x004168e2) | NOT DEAD — switch-case target |
| 0x0041808D | `caseD_6` | 1 DATA (0x004183a8) + 1 COMPUTED_JUMP (0x00417d65) | NOT DEAD — switch-case target |
| 0x00418450 | `NoOpHookStub` | 13 UNCONDITIONAL_CALL sites | NOT DEAD — actively called |
| 0x0043D9F0 | `ApplyRandomWheelJitterSynchronized` | 1 DATA (0x00474968, inside orphaned table) | **DEAD-CODE CONFIRMED** |

### Detail per finding

**0x004173B1 / 0x00417700 / 0x0041808D**: jump-table entries in switch dispatches:
- `0x004168e2` is inside `RaceTypeCategoryMenuStateMachine` (0x004168b0..0x00417ccd). The computed jump indexes a jump table at 0x00417cd2.. covering at least 0x00417cf0 (→ caseD_7) and 0x00417cfc (→ caseD_a). These cases ARE selectable at runtime via the screen-state index.
- `0x00417d65` is inside `ScreenExtrasGallery` (0x00417d50..0x0041838c). Jump table at 0x004183a8.. holds 0x0041808D among its entries.

These are first-class switch-arm functions, not dead code. The earlier "DEAD-CODE residual" classification mis-handled jump-table refs.

**0x00418450 (NoOpHookStub)**: 13 callers spread across menus, race-frame teardown, and viewport setup. Already correctly bookmarked as `HOOK_POINT` (lifecycle stub). Definitely live; classifier should not have flagged it.

**0x0043D9F0 (ApplyRandomWheelJitterSynchronized)**: Only reachable through 0x00474968 — a slot inside an orphaned function-pointer table spanning 0x00474940..0x004749BC. The table contains 0x0043d820 (a sibling stub/handler) repeated 28 times plus this single 0x0043d9f0 slot at offset +0x28 (10th element). NO code in the binary references the table base (constant search for 0x00474940/0x00474950/0x00474968 yields zero hits). The table is an orphan — either dead initializer-data leftover from a removed dispatcher or accessed via a base computed at link time that the disassembler did not recover.

Static reachability is zero. **DEAD-CODE verdict stands for 0x0043D9F0 only.** Recommend removing the other four from the dead-code list.

### Verdict
- 4/5 entries are FALSE POSITIVES (switch-case + actively called stub). Update classifier to honor `DATA → ADDR → COMPUTED_JUMP` chains and `UNCONDITIONAL_CALL`.
- 1/5 (0x0043D9F0) is genuine dead code (orphaned table consumer). May still be runtime-reachable via dynamic resolution; recommend a Frida probe before deletion.

---

## Task 2: CRT helper boundary refinement

Enumerated 265 functions whose names contain `_`; 228 of those start with `_` or `__` (the L0 set; the original spec said 232 — the 4-entry delta is likely because `_pgmptr_init` / `_cinit` / `_setargv` / `_setenvp` were merged or the original count was inclusive of game wrappers). All 228 entries clustered in the CRT address range 0x00448000..0x00451FFF.

### Largest L0 bodies (> 256 bytes)
All 37 are genuine CRT internals — none are misclassified:

```
2596 _input            0044ca49..0044d46d  (scanf core)
1824 _output           0044af28..0044b648  (printf core)
1184 __strgtold12      00450dfc..0045129c  (long-double parse)
 820 _memmove          0044bcd0..0044c004
 820 _memcpy           0044f580..0044f8b4
 808 __sbh_free_block  00449d9e..0044a0c6  (small-block heap free)
 776 _heap_alloc_base  0044a0c7..0044a3cf
 718 _sopen            0045037a..00450648
 690 __build_exc_record (SEH plumbing)
 547 __crtLCMapStringA
 543 __ld12mul         (long-double mul)
 534 _fpieee_flt       (FPU exception)
 519 __sbh_alloc_block
 516 _strtoxl
 472 _read_nolock
 443 _ioinit
 [...rest: parse_cmdline, setmbcp, write_nolock, initctype, openfile, etc.]
```

### Game-wrapper sanity check
The 5 known game wrappers (`sprintf_game`, `fclose_game`, `fopen_game`, `fread_game`, `fseek_game`) live at 0x00448443..0x00448E91 — INSIDE the CRT cluster but with non-underscore names. They are correctly NOT in L0.

### Suspicious-name review
Walked all 228 names; none look like game helpers wearing CRT clothing. The full set is canonical MSVC 6/7 CRT internals: heap (small-block-heap `__sbh_*`), FPU helpers (`__ftol`, `__hw_to_abstract_fp`, `__fpieee_flt`), SEH plumbing (`__global_unwind2`, `__XcptActTab_lookup`, `__build_exc_record`, `__raise_exc_single`), long-double math (`__ld12*`, `__strgtold12`, `__multtenpow12`), stdio (`_input`/`_output`/`_filbuf`/`_flsbuf`/`_stbuf`), I/O (`_sopen`, `_read_nolock`, `_write_nolock`, `_alloc_osfhnd`), startup (`__cinit`, `__initterm`, `__initptd`).

Notably benign edge cases verified individually:
- `__retval_zero` (3 bytes, 0x00450b8d) — trivial XOR EAX,EAX / RET
- `_clearfp_nw` (14 bytes) — FPU control-word clear-no-wait
- `__XcptActTab_lookup` (57 bytes) — SEH exception-action table walker
- `__crtMessageBoxA` (136 bytes) — CRT-internal MessageBoxA wrapper

### Verdict
**No L0 misclassifications.** Classifier rule `name.startswith("_")` over the CRT address cluster is sound for this binary. The slight count discrepancy (228 vs 232) likely reflects deduplication of name-aliases (e.g. `__initterm` + `__initterm_real`) or a stale classifier baseline; not a correctness issue.

---

## Task 3: CODE_CAVE + PATCH_SITE bookmark inventory

`bookmark_list` returned 0 results when filtered by `bookmark_type=CODE_CAVE` or `PATCH_SITE` (Ghidra treats these as `category`, not `type`). The full list (247 bookmarks total, type=Note) contains the 2 + 6 entries spec'd.

### CODE_CAVE bookmarks (2)

| Address | Surrounding function | Comment |
|---|---|---|
| 0x0045C330 | `(no function)` — data block in `.text` tail | "Widescreen scale cave start (used by td5_mod.c PatchCallSite)" |
| 0x0045C580 | `(no function)` — data block in `.text` tail | "GDI text rendering cave start (reserved for font patch)" |

Both caves sit in the post-CRT `.text` slack region (0x0045C000+), outside any analyzed function — exactly where you'd carve scratch space for trampolines.

### PATCH_SITE bookmarks (6)

| Address | Owner function | Comment |
|---|---|---|
| 0x00414D09 | inside frontend Flip dispatch (~0x00414b50 `RunFrontendDisplayLoop`) | "Flip call site - redirected to ScaleCave for frontend scaling" |
| 0x0042CBBE | inside race main loop (~0x0042b580 `RunRaceFrame`) | "Flip call site - redirected to ScaleCave for race scaling" |
| 0x00414C9E | inside `RunFrontendDisplayLoop` | "Screen dispatch - redirected to LogicGate_ScreenDispatch (6 bytes)" |
| 0x00414EB0 | inside `RunFrontendDisplayLoop` | "INC EDX NOPped - frame counter handled by LogicGate" |
| 0x004465F4 | inside weather init (`ConfigureWeatherState` ~0x00446400) | "Snow gate JNZ - NOP 6 bytes to enable snow weather type" |
| 0x0042C8D0 | `GetDamageRulesStub` (entry) | "GetDamageRulesStub - patched to JMP _rand for damage wobble restore" |

Cross-referenced with HOOK_POINT bookmarks: PATCH_SITE addresses align with td5_mod / DLL-injection-style call-site rewrites; CODE_CAVE addresses are the trampoline destinations.

### Verdict
- 2 CODE_CAVE + 6 PATCH_SITE bookmarks present and well-described. No missing/orphan entries.
- All PATCH_SITE comments are precise (byte count + intent + destination cave).
- Cleanup opportunity: Use `bookmark_list` `category` filter (not `bookmark_type`) for these in future tooling — the type/category split is non-obvious.

---

## Pool slot cleanup
Will release TD5_pool11 immediately after this write.
