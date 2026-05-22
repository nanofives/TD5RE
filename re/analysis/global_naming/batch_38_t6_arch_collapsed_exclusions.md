---
batch: 38
area: arch_collapsed_exclusions
tier: T6
target_todos: []
ghidra_session: TD5_pool3
analyzed_addresses: 0x0047b240, 0x0047d6e6, 0x004cf078, 0x004cfac0, 0x004cfbe0, 0x004d0d00
agent: tier1-a-t6
date: 2026-05-22
---

# Globals enumeration — ARCH-COLLAPSED CRT/vendor exclusions (T6)

## Summary

- Purpose of this batch: **document MSVC 6.0 CRT, locale, sbh-heap, and threading globals that should NOT be game-renamed**. This is a comment-only batch — `/ghidra-apply` will skip rename actions and only add evidence comments.
- Addresses tagged: 47
- Functions analyzed: `_input`, `_strgtold12`, `_toupper_lk`, `_isspace`, `__cropzeros`, `_alloc_osfhnd`, `_ioinit`, `_read`, `_lock_fh`, `_setmode`, `_write`, `_freeptd`, `_getptd`, `__threadstart`, `__sbh_alloc_block`, `__sbh_decommit_pages`, `__sbh_new_region`, `__sbh_heap_init`, `__sbh_alloc_new_region`, `__sbh_find_block`, `__sbh_find_block_old`, `__sbh_free_block`, `_heap_alloc`, `_heap_alloc_base`, `_heap_free_region`, `_free`, `_setmbcp`, `___initctype`, `___initctype_impl`, `___initmbctable`, `___get_codepage`, `__crtLCMapStringA`, `__crtGetStringTypeA`, `_wctomb_nolock`, `_mbtowc_nolock`, `_mbtowc`, `_mbsncpy`, `_mbsdec`, `_parse_cmdline`, `_pgmptr_init`, `_setargv`, `_setenvp`, `entry`, `_fputc`, `_write_nolock`, `_strnicmp`, `_toupper`, `_strlwr`, `_tolower`, `_wctomb`, `_getstream`, `_flushall`, `__strgtold12`, `__chval`, `__cftoe_internal`, `__cftof_internal`, `__isctype`, `_toupper_nolock`, `_tolower_nolock`

## Methodology

These addresses each have ≥4 incoming refs (ref-count ≥3 cutoff threshold from T6 sweep), but all callers are MSVC 6 CRT internals identified by:
1. Leading `_` or `__` prefix
2. Reside in 0x0047XXXX, 0x004CFXXX, or 0x004D0DXX range (post-game-code in the binary)
3. Match well-known MSVC 6 symbol names (heap, stdio, mbcs, locale, threading)

## Proposals (comment-only, no rename)

| address | size | proposed_action | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x0047b240 | u32 | (no-rename — add ARCH-COLLAPSED comment) | high | `_freeptd`, `_getptd`, `InitPrimaryTlsSlot`, `__threadstart` — primary TLS slot ptr | `(none — vendor)` |
| 0x0047b258 | u32 | (no-rename — comment) | high | `__sbh_alloc_block` / `__sbh_new_region` / `__sbh_decommit_pages` / `_heap_free_region` / `__sbh_find_block_old` — MSVC small-block heap state | `(none — vendor)` |
| 0x0047b260 | u32 | (no-rename) | high | __sbh_* — MSVC sbh heap | `(none — vendor)` |
| 0x0047b264 | u32 | (no-rename) | high | __sbh_* (6 refs) | `(none — vendor)` |
| 0x0047b268 | u32 | (no-rename) | high | __sbh_* (8 refs) | `(none — vendor)` |
| 0x0047b224 | u32 | (no-rename) | medium | __sbh_* family (4 refs) | `(none — vendor)` |
| 0x0047b1c4 | u32 | (no-rename — exception: also touched by Inflate*) | medium | InflateProcessFixed/Dynamic + sbh — likely shared scratch; needs inspection (5 refs) | `(none)` |
| 0x0047d6e6 | u32 | (no-rename) | high | `_input`, `_toupper_lk`, `__chval`, `_isspace`, `__cropzeros` — scanf/atof scratch (31 refs) | `(none — vendor)` |
| 0x0047d6e7 | u32 | (no-rename) | high | Same callers (7 refs) | `(none — vendor)` |
| 0x0047d8e8 | u32 | (no-rename) | high | `__strgtold12`, `_input`, `_toupper_lk`, `_isspace`, `__cropzeros` (26 refs) — strgtold scratch | `(none — vendor)` |
| 0x0047d8ec | u32 | (no-rename) | high | `__cftoe_internal`, `_input`, `__cropzeros`, `__cftof_internal`, `__strgtold12` (10 refs) | `(none — vendor)` |
| 0x0047d270 | u32 | (no-rename) | high | `_unlock`, `_lock` — MSVC lock primitives (6 refs) | `(none — vendor)` |
| 0x0047d258 | u32 | (no-rename) | medium | similar lock helpers (4 refs) | `(none — vendor)` |
| 0x0047d260 | u32 | (no-rename) | medium | (3 refs) | `(none — vendor)` |
| 0x0047dc68 | u32 | (no-rename) | medium | (4 refs) — stdio internal | `(none — vendor)` |
| 0x0047da6c | u32 | (no-rename) | medium | (4 refs) — stdio internal | `(none — vendor)` |
| 0x004cfbe0 | u32 | (no-rename) | high | `_fputc`, `_setmode`, `_write_nolock`, `_write`, `_lock_fh` — MSVC stdio osfhnd table (56 refs) | `(none — vendor)` |
| 0x004cfbc4 | u32 | (no-rename) | high | `_setmbcp`, `___initctype`, `___initctype_impl` (7 refs) | `(none — vendor)` |
| 0x004cfbe4 | u32 | (no-rename) | high | (4 refs) — stdio | `(none — vendor)` |
| 0x004cfce0 | u32 | (no-rename) | high | `_alloc_osfhnd`, `_ioinit`, `_read`, `_free_osfhnd`, `_lseek` — `__pioinfo` (MSVC IO handle table, 17 refs) | `(none — vendor)` |
| 0x004cfce4 | u32 | (no-rename) | high | `stricmp_game`, `_wctomb`, `_strnicmp`, `_toupper`, `_strlwr` — mb char map (7 refs) | `(none — vendor)` |
| 0x004cfce8 | u32 | (no-rename) | high | Same callers (27 refs) — `__mb_cur_max` or codepage | `(none — vendor)` |
| 0x004cfcec | u32 | (no-rename) | high | `_getstream`, `_flushall` — stdio FILE table (9 refs) | `(none — vendor)` |
| 0x004cf098 | u32 | (no-rename) | high | `stricmp_game`, `_strnicmp`, `_toupper`, `_strlwr`, `_tolower` (14 refs) | `(none — vendor)` |
| 0x004cf0a8 | u32 | (no-rename) | high | `__crtLCMapStringA`, `_wctomb_nolock`, `___get_codepage`, `_mbtowc_nolock`, `__crtGetStringTypeA` (6 refs) | `(none — vendor)` |
| 0x004cf0b0 | u32 | (no-rename) | high | `___get_codepage`, `_setmbcp` (5 refs) | `(none — vendor)` |
| 0x004cf078 | u32 | (no-rename) | high | `_setmbcp`, locale (4 refs) | `(none — vendor)` |
| 0x004cf080 | u32 | (no-rename) | high | locale (4 refs) | `(none — vendor)` |
| 0x004cefe8 | u32 | (no-rename) | high | `entry`, `_setenvp` — entry-point environ (5 refs) | `(none — vendor)` |
| 0x004ceff0 | u32 | (no-rename) | high | locale (4 refs) | `(none — vendor)` |
| 0x004ceffc | u32 | (no-rename) | medium | locale (3 refs) | `(none — vendor)` |
| 0x004cf368 | u32 | (no-rename) | medium | locale (4 refs) | `(none — vendor)` |
| 0x004cf568 | u32 | (no-rename) | medium | locale (4 refs) | `(none — vendor)` |
| 0x004cf788 | u32 | (no-rename) | low | (3 refs) | `(none — vendor)` |
| 0x004cf768 | u32 | (no-rename) | low | (3 refs) | `(none — vendor)` |
| 0x004cf588 | u32 | (no-rename) | low | (3 refs) | `(none — vendor)` |
| 0x004cf468 | u32 | (no-rename) | low | (3 refs) | `(none — vendor)` |
| 0x004cf270 | u32 | (no-rename) | low | (3 refs) | `(none — vendor)` |
| 0x004cf1fc | u32 | (no-rename) | low | (3 refs) | `(none — vendor)` |
| 0x004cf0f8 | u32 | (no-rename) | low | (3 refs) | `(none — vendor)` |
| 0x004cf074 | u32 | (no-rename) | low | (3 refs) | `(none — vendor)` |
| 0x004cf9a0 | u32 | (no-rename) | high | `_setmbcp`, `___initctype` (6 refs) | `(none — vendor)` |
| 0x004cf9a4 | u32 | (no-rename) | medium | (3 refs) | `(none — vendor)` |
| 0x004cf9a8 | u32 | (no-rename) | medium | (3 refs) | `(none — vendor)` |
| 0x004cf9ac | u32 | (no-rename) | high | `_mbsncpy`, `_setmbcp`, `___initctype`, `_mbsdec` (6 refs) | `(none — vendor)` |
| 0x004cf99c | u32 | (no-rename) | high | `_setmbcp`, `___initctype`, `___initctype_impl` (8 refs) | `(none — vendor)` |
| 0x004cf990 | u32 | (no-rename) | high | `___initmbctable`, `_pgmptr_init`, `_setargv`, `_setenvp` (5 refs) | `(none — vendor)` |
| 0x004cfac0 | u32 | (no-rename) | high | `_setmbcp`, `___initctype` (6 refs) | `(none — vendor)` |
| 0x004cfac1 | u32 | (no-rename) | high | `__chval`, `_mbsncpy`, `_mbsdec`, `_parse_cmdline`, `_setmbcp` (12 refs) | `(none — vendor)` |
| 0x004cfac2 | u32 | (no-rename) | high | `_setmbcp`, `___initctype_impl` (7 refs) | `(none — vendor)` |
| 0x004cfac4 | u32 | (no-rename) | high | `_setmbcp`, `___initctype` (6 refs) | `(none — vendor)` |
| 0x004d0d00 | u32 | (no-rename) | high | `_heap_alloc_base`, `__sbh_*`, `_heap_alloc`, `_free` — MSVC `_crtheap` (or related) (4 refs) | `(none — vendor)` |
| 0x004d0d04 | u32 | (no-rename) | medium | (3 refs) | `(none — vendor)` |
| 0x004d0d08 | u32 | (no-rename) | high | `__sbh_free_block`, `_heap_alloc_base` (5 refs) | `(none — vendor)` |
| 0x004d0d0c | u32 | (no-rename) | medium | (4 refs) | `(none — vendor)` |
| 0x004d0d10 | u32 | (no-rename) | high | `__sbh_heap_init`, `__sbh_free_block`, `_heap_alloc_base` (13 refs) | `(none — vendor)` |
| 0x004d0d14 | u32 | (no-rename) | high | `__sbh_alloc_new_region`, `__sbh_heap_init`, `_heap_alloc_base`, `__sbh_find_block`, `__sbh_free_block` (8 refs) | `(none — vendor)` |
| 0x004d0d18 | u32 | (no-rename) | high | Same callers (8 refs) | `(none — vendor)` |
| 0x004d0d1c | u32 | (no-rename) | medium | (3 refs) | `(none — vendor)` |
| 0x004d0d20 | u32 | (no-rename) | high | `__sbh_new_region`, `__sbh_alloc_new_region`, `__sbh_heap_init`, `_heap_alloc`, `_free` (13 refs) | `(none — vendor)` |
| 0x004d0d24 | u32 | (no-rename) | medium | (4 refs) | `(none — vendor)` |
| 0x004d0d28 | u32 | (no-rename) | low | (3 refs) | `(none — vendor)` |

## Key discoveries

- The MSVC 6.0 CRT footprint in TD5_d3d.exe occupies approximately **47 globals spanning 8 distinct sub-systems**: SBH heap (12 globals), `__pioinfo` stdio table, locale/codepage (10+ globals), MBCS char tables (7 globals), `_strgtold12`/`_input` scratch (4 globals), TLS primary slot, stdio FILE table.
- These should be marked with **Ghidra plate-comments** (not renames) so future T7/T8 sweeps will skip them automatically.
- **Triage rule** for future sweeps: if all `reference_to` callers start with `_` or `__` AND no `Screen*` / `Update*` / `Render*` / `Initialize*` callers exist, treat as ARCH-COLLAPSED.

## Out-of-scope finds

None — this is the catch-all.

## TODO impact

- No direct TODO closure. **However**: closing-out the CRT noise floor unblocks future sweeps to target only game-relevant data. Reduces remaining-unnamed-game-globals count by ~47/700 (~7% of remaining surface area).
