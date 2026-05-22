---
batch: NN
area: <short area name>
tier: T1
target_todos: [list of related todo memory slugs]
ghidra_session: <session_id>
analyzed_addresses: <comma-separated function addresses>
agent: <agent identifier>
date: 2026-05-20
---

# Globals enumeration — <area>

## Summary

- Functions analyzed: N
- Unnamed DAT_* globals encountered: M (after de-dup)
- Already-named globals encountered (just noted): K
- Proposals — high confidence: A
- Proposals — medium confidence: B
- Proposals — comment-only (low confidence): C

## Methodology

Briefly: which functions were the entry points, how did you walk callers/callees, what was the gate for "this global is relevant to this area"?

## Proposals (globals)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0xXXXXXXXX | u8/u16/u32/struct | `g_proposed_name` | high/med/low | one-line: who writes it, who reads it, semantic basis | path/to/port/file.c:NNN or `(none)` |

## Proposals (functions) — *optional, only if the batch renames functions*

| address | current_name | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00XXXXXX | FUN_004XXXXX (or autoname) | `Verb_Noun` | high/med/low | one-line: callers, callees, role | path/to/port/file.c:NNN or `(none)` |

Notes on the confidence levels (applies to both globals and functions):

- **high** → `/ghidra-apply` will apply the rename + add a Ghidra comment with the evidence
- **medium** → apply name with `_PROVISIONAL` suffix
- **low** → add a Ghidra comment with the analysis only; leave the original label

Either table may be omitted. `/ghidra-apply` processes whichever sections exist.

## Key discoveries

Bulleted list of non-obvious flow/control facts you uncovered while doing this analysis. These are NOT global-naming proposals — they're insights about how the original game's state machine works that we'd otherwise miss. Examples:

- "Field +0x378 is recomputed per-tick from input bit 28, NOT seeded from cardef" (overturns a TODO assumption)
- "The screen-table dispatch in `0xXXXX` doesn't use g_frontendScreen directly, it reads through an intermediate `g_pendingScreen` that gates state transitions"

## Out-of-scope finds

Unnamed globals you spotted but that belong to a different T1 area or a future tier. List them so the next batch covers them.

| address | brief note | probable area |
|---|---|---|

## TODO impact

For each related TODO from the frontmatter, a short verdict:

- **todo_xxx_2026-05-19**: Closes via finding W. Suggests fix at port file/line F.
- OR: Investigation surfaces no closing mechanism in this area; revisit in tier T2 or T3.
