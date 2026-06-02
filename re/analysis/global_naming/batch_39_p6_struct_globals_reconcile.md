---
batch: 39
area: p6_struct_globals_reconcile
tier: T6
target_todos: []
ghidra_session: read-only verification only (master TD5)
analyzed_addresses: 0x00495234, 0x004aee14, 0x004aee54, 0x004962a4
agent: Workstream-F / Session P6
date: 2026-06-01
---

# Globals enumeration — P6 struct/global/format reconciliation

## Summary

- Functions analyzed: 0 new (xref re-verification of 4 known globals)
- Unnamed DAT_* globals encountered: 0
- Already-named globals encountered (reconciled): 4
- Proposals — high confidence: 4
- Proposals — medium confidence: 0
- Proposals — comment-only (low confidence): 0

This is a **naming-hygiene** batch: every address already has a symbol. The work is to
(a) collapse a CPUID-origin duplicate to its real extras-gallery role, (b) confirm the
lighting-zone primary against a disproven older guess, (c) drop a `_PROVISIONAL` suffix
that is now confirmed, and (d) stamp a "DEAD (write-only)" comment on a latch that has
no reader. **No port code changes** result from any of these.

## ✅ APPLIED to master TD5.gpr 2026-06-02

All 4 applied in one transaction (backup `TD5.rep.bak.20260602-002216-pre-apply`, saved, master
closed, pool synced). **Live inspection corrected three of the brief's assumptions:**

1. **0x00495234** had NO `g_cpuHasMmx` duplicate to collapse — only `g_extrasGalleryEnabledFlag_PROVISIONAL`
   existed. Action was a plain rename → `g_extrasGalleryEnabledFlag` (drop `_PROVISIONAL`); CPUID-MMX
   origin captured in the EOL comment. No `symbol_primary_set` needed.
2. **0x004aee14** literally held the disproven name `g_trackCircuitFlagOrLapCount_PROVISIONAL` →
   renamed to `g_trackLightingZoneTablePtr`.
3. **0x004aee54** real name was `g_modelsDatPostEntryTableBlob_PROVISIONAL` (NOT `...End` as the brief
   said) → `g_modelsDatPostEntryTableBlob`. Decompile confirmed it = `gModelsDatEntryTable + entryCount*8`
   (one past the entry table), write-only; reconfirmed MODELS.DAT dword-1 is never touched.

`g_tgaLoadFailureLatch` @0x004962a4 already had the right name → comment-only (DEAD, 15W/0R). All four
were simple renames/comments; none needed the non-standard `symbol_primary_set`/collapse ops anticipated
below. **Pool propagation:** slot 0 refreshed; slot 1 hit the known "Device or resource busy / `.gbf`"
incident (needs a reboot + re-sync); slots 2–15 not yet refreshed by the aborted run — re-run
`scripts/ghidra_pool.sh sync` after a reboot to propagate the new names to all read-only slots. Master
is authoritative regardless.

## Methodology

Read-only re-verification against the master `TD5` Ghidra project (pool slots were
locked; documented master fallback, `read_only=true`). For each address, `reference_to`
enumerated every xref and classified read vs write; the containing function names were
checked to establish the semantic role. All counts below are literal xref tallies.

## Proposals (globals)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00495234 | u32 | `g_extrasGalleryEnabledFlag` | high | 7 xrefs: 2 WRITE (0x004147c3, 0x0042507c) + 5 READ all in extras-gallery fns (0x0040d590 LoadExtrasGalleryImageSurfaces, 0x0040d640 ReleaseExtrasGalleryImageSurfaces, 0x0040d84c/0x0040da3a/0x0040da65 UpdateExtrasGalleryDisplay). Role (extras gallery) outranks the CPUID-origin `g_cpuHasMmx` alias. | `(none — extras gallery)` |
| 0x004aee14 | u32 (ptr) | `g_trackLightingZoneTablePtr` | high | 1 WRITE LoadTrackRuntimeData @0x0042fd19; 2 READ lighting fns @0x0040cdaa, @0x00430150. Confirms lighting-zone-table role; **disproves** the older `g_trackCircuitFlagOrLapCount` guess. | `re/analysis/formats/data-structure-gaps-filled.md §5` |
| 0x004aee54 | u32 (ptr) | `g_modelsDatPostEntryTableBlob` | high | Single WRITE 0x0043119e (ParseModelsDat) = `gModelsDatEntryTable + entryCount*8`, 0 READ. Write-only ptr to the blob just past the entry table. **Dropped the `_PROVISIONAL` suffix** — confirmed. (Real name was `...Blob`, not `...End` as the brief said.) | `(none — write-only)` |
| 0x004962a4 | u32 | `g_tgaLoadFailureLatch` | high | 15 xrefs, **all WRITE** (0x00411fad … 0x00412c48), 0 READ. Sticky error latch that is never consumed. Name kept; add DEAD comment. The port propagates TGA load failures via return codes, not this latch. | `(none — DEAD)` |

## Apply notes (NON-STANDARD ops — do not blind-rename)

The generic `/ghidra-apply` flow (`symbol_rename` + `comment_set`) does **not** cleanly cover
two of these. When applying (master `read_only=false`, the documented HARD-RULE exception),
inspect the current symbol set at each address first (`symbol_list` / `symbol_by_name`) and use
the right op:

1. **0x00495234 — collapse duplicate.** If both `g_extrasGalleryEnabledFlag` and `g_cpuHasMmx`
   exist at this address, make `g_extrasGalleryEnabledFlag` the **primary** (`symbol_primary_set`),
   and either delete the `g_cpuHasMmx` alias or keep it; either way add an **EOL comment**:
   `"CPUID-MMX origin; runtime role is extras-gallery enable flag (5 reads all extras fns) [batch_39 2026-06-01]"`.
   Do NOT plain-rename (would collide on the existing duplicate name).

2. **0x004aee14 — confirm primary.** If already `g_trackLightingZoneTablePtr`, this is an
   idempotent no-op + comment. If a stale `g_trackCircuitFlagOrLapCount` symbol is present,
   set the lighting-zone name primary and comment the disproof.

3. **0x004aee54 — rename.** `g_modelsDatPostEntryTableEnd_PROVISIONAL` → `g_modelsDatPostEntryTableEnd`
   (clean rename) + EOL comment with the 1-write/0-read evidence.

4. **0x004962a4 — comment-only.** Name already correct; just add the DEAD evidence comment.

## Key discoveries

- `g_tgaLoadFailureLatch` @0x004962a4 is a **write-only sticky latch (15W/0R)** — it is set on
  every TGA load failure but never read. Confirms the port's decision to surface load failures
  via return codes is faithful (the latch carried no behavior).
- `g_modelsDatPostEntryTableEnd` @0x004aee54 is **write-only (1W/0R)** — a bookkeeping pointer set
  at the end of `ParseModelsDat`'s entry-table relocation, never consumed. Vestigial; do not
  implement.

## Out-of-scope finds

| address | brief note | probable area |
|---|---|---|
| `g_textureUsesTallPageFormat_PROVISIONAL` | gate read in LoadStaticTrackTextureHeader @0x442560 selecting tall-page width/height order; still `_PROVISIONAL`. | static.hed / texture loaders |

## TODO impact

- P6 audit (struct/global/format reconciliation): these 4 close the Ghidra-naming half. The doc
  half is applied in `re/analysis/actor_struct_gap_audit_2026-05-20.md`,
  `re/analysis/formats/data-structure-gaps-filled.md`,
  `re/analysis/followup_sessions/wave1_f_overview.md`, and
  `re/analysis/audits/function-mapping-confidence-audit-2026-04-08.md` (all 2026-06-01).
