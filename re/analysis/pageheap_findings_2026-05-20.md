# PageHeap audit on TD5_d3d.exe — first finding

**Date:** 2026-05-20
**Method:** Application Verifier (Full Page Heap, Backwards-mode) on `TD5_d3d.exe`, launched under WinDbg via the windbg MCP. First-chance access violation captured at boot.

## Finding #1 — heap-underflow read in `LoadStaticTrackTextureHeader`

**Function:** `LoadStaticTrackTextureHeader @ 0x00442560` (body 0x00442560–0x00442664)
**Faulting instruction:** `0x004425EA — mov edx, dword ptr [ecx - 4]`
**Caller chain:** `LoadStaticTrackTextureHeader` ← `InitializeRaceVideoConfiguration @ 0x0042A950` ← `0x00430C16` ← `0x004494C0`

**Register state at trap:**
- `EIP = 0x004425EA`
- `ECX = 0x3CAA2004` (start of a Page Heap allocation; this is `puVar5`)
- `ECX - 4 = 0x3CAA2000` (PAGE_NOACCESS guard page — trap)
- `EBX = 0x0C79EFF0` (likely `_Memory` from the `_malloc` call)
- `EAX = 0`, `EDX = 0`, `ESI = 2`

**Decompiled context (Ghidra):**

```c
void LoadStaticTrackTextureHeader(void) {
    // ...
    uVar3 = GetArchiveEntrySize(s_static_hed, s_static_zip);
    _Memory = _malloc(uVar3);
    ReadArchiveEntry(s_static_hed, s_static_zip, _Memory, uVar3);
    gTrackTextureCount     = *_Memory;          // header[0]
    gStaticHedEntryCount   = _Memory[1];        // header[1]
    iVar2 = gStaticHedEntryCount * 0x10;
    gStaticHedTextureData  = _Memory + iVar2 + 2;
    // ...
    if (gTrackTextureCount != 0) {
        puVar5 = _Memory + iVar2 + 2 + 3;       // start at offset +3 past texture data
        do {
            if (DAT_004c3d04 == 0) {
                uVar6 = puVar5[-1];             // ← reads element BEFORE puVar5
                uVar1 = *puVar5;
                // ... min(uVar1, uVar6) ...
            }
            else {
                uVar6 = *puVar5;
                uVar7 = puVar5[-1];             // ← same read on other branch
            }
            // store, accumulate, advance
            puVar5 = puVar5 + 4;                // advance 16 bytes per texture
            // ...
        } while (uVar3 < gTrackTextureCount);
    }
}
```

The trap happens because under Backwards Page Heap, `_malloc(...)` placed the allocation flush against the start of a page (guard page immediately before). Whatever offset the loop produced (`puVar5 == _Memory` in this case, suggesting `iVar2 = -20 bytes`, which Ghidra's type recovery garbled), the `[puVar5 - 4]` read hits the guard page.

## What this means

This is a **byte-OOB read in orig** that has been silently working because:
- Normal Windows heap (without PageHeap) places allocations with heap-metadata prefix bytes in front. Reading `_Memory[-1]` picks up part of that prefix.
- The orig has likely been doing this for the entire game's life — the result is non-zero but unspecified.

For the port:
- MinGW's malloc (and similar) places different prefix bytes. The port reads different values.
- Likely effect: ONE texture entry's pixel-count gets a "wrong" first-element comparison. The min/max scan picks a slightly different value.
- **Impact on convergence: probably small.** This is in texture-metadata loading (Set_exref + 4 = total pixel count for the static texture set). Affects the LogReport string and possibly memory allocation sizing for textures, but doesn't propagate into physics/AI/cascade math.

## What this does NOT find

The PageHeap audit captured exactly one first-chance trap before the verifier kept re-triggering on the same instruction (the loop fires on every iteration). To find traps in physics/AI/cascade code we'd need to:
1. NOP-patch this read so the boot completes, OR
2. Skip the loop via WinDbg `r eip = 0x...` to advance past it, OR
3. Disable PageHeap on this specific function (Application Verifier doesn't support per-function granularity)

Most practical: temporarily set Application Verifier's heap detection to "Standard" (not Full Page Heap) — this fills allocations with `0xC0` pattern but doesn't put guard pages, so reads succeed (with the fill bytes) instead of trapping. Then we'd observe behavior changes (which would be the bug signature) without process termination.

## Recommendation

1. **Low priority to fix** — texture pixel-count off-by-one is cosmetic from a convergence-residue perspective.
2. **Document for completeness** — see memory entry `reference_pageheap_loadstatictracktextureheader_2026-05-20`.
3. **To find more impactful uninit reads**: switch to Standard PageHeap mode in appverif (uncheck "Full"), re-run; the game won't crash but adjacent memory will be filled with `0xC0C0C0C0`. Compare a runtime snapshot with PageHeap on vs. off — fields that change values are the uninit-read consumers.

## Cleanup reminder

Application Verifier's PageHeap on `TD5_d3d.exe` remains active until you explicitly remove it. Otherwise every future launch slows the game and may hit this same trap. To remove:
1. Open `appverif.exe` as Administrator
2. **File → Delete Application** (select `TD5_d3d.exe`) OR uncheck **Basics → Heaps** and **File → Save**
