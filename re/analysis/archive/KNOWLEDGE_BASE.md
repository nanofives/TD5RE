# Knowledge Base

Promote stable findings here once they are confirmed.

## How to use this file

- Keep entries short and factual.
- Prefer concrete addresses, names, structures, and constraints.
- Link each fact back to a session note when possible.
- Do not mix guesses with confirmed findings.

## Entry template

### Topic

- Binary:
- Address or range:
- Summary:
- Evidence:
- Related session:
- Notes:

### TD5 EXE startup chain

- Binary: `TD5_d3d.exe`
- Address or range: `0x004493E0 -> 0x00430A90 -> 0x00430B54`
- Summary: PE entry at `0x004493E0` is CRT/setup code that prepares `WinMain`; the game-owned startup handoff is `0x00430A90`, and the first stable startup/message loop sits at `0x00430B54`.
- Evidence: PE header inspection shows entry RVA `0x493E0`; disassembly export shows `GetVersion`, `GetCommandLineA`, `GetStartupInfoA`, `GetModuleHandleA(NULL)`, then a call to `0x00430A90`; `WinMain` then calls `DXWin::Environment`, writes startup defaults into `DX::app`, calls `DXWin::Initialize`, and enters the message loop.
- Related session: `analysis-artifacts/sessions/2026-03-17-startup-entry-chain.md`
- Notes: Best next function target is `0x00442160`, the first game-owned callee reached after `DXWin::DXInitialize`.

### DX::app ownership in the EXE startup path

- Binary: `TD5_d3d.exe`
- Address or range: `0x0045D53C`
- Summary: `0x0045D53C` is imported data symbol `?app@DX@@2UtagDX_APP@@A` from `M2DX.dll`, not an EXE-owned pointer slot.
- Evidence: import-table reconstruction maps `0x0045D53C` to the M2DX data import; `WinMain` writes startup fields through this address during the initial video/app setup sequence.
- Related session: `analysis-artifacts/sessions/2026-03-17-startup-entry-chain.md`
- Notes: Confirmed startup writes include `+0xBC=640`, `+0xC0=480`, `+0xC4=16`, `+0x20=0x0045D6B0`, and `+0xC8=nCmdShow`.

### TD5 startup dispatcher states

- Binary: `TD5_d3d.exe`
- Address or range: `0x00442170`, jump table `0x00442550`, state global `0x004C3CE8`
- Summary: `0x00442170` is the four-state runtime dispatcher reached from `WinMain`: state `0` bootstrap, state `1` frontend, state `2` race/runtime, state `3` file-backed image presentation.
- Evidence: the function bounds-checks `0x004C3CE8` to `0..3` and dispatches to `0x00442195`, `0x004421F3`, `0x0044227F`, and `0x004422EC`; those branches call `InitializeMemoryManagement`/`SetRenderState`, `0x00414B50`, `0x0042B580`, and a TGA/image decode + `Flip` path respectively.
- Related session: `analysis-artifacts/sessions/2026-03-17-startup-state-dispatcher.md`
- Notes: `0x00442160` beside it is only a thunk to `0x0042A950`.

### Runtime transition bootstrap after startup dispatcher

- Binary: `TD5_d3d.exe`
- Address or range: `0x0042A950`, `0x0042AA10`, `0x0042B580`
- Summary: `0x0042A950` is a thin bootstrap shim; `0x0042AA10` is the real frontend-to-runtime transition initializer; `0x0042B580` is the first runtime frame owner after that transition.
- Evidence: `0x0042A950` only seeds globals, calls `0x0042C2B0` / `0x00442560`, and installs callback `0x004285B0`; `0x0042AA10` loads `LOADING.ZIP`, `load%02d.tga`, `MODELS.DAT`, `STATIC.ZIP`, and `SKY.PRR`, creates a private heap, configures DXInput, and seeds six-entry table `0x004AADF4`; `0x0042B580` consumes that table and drives the next runtime frame/update path.
- Related session: `analysis-artifacts/sessions/2026-03-17-runtime-transition-bootstrap.md`
- Notes: This layer is now decomposed further in `analysis-artifacts/re/analysis/runtime_slot_table.md`; the best next deep target under it is `0x00402E60`.

### Runtime slot table and selected-slot roots

- Binary: `TD5_d3d.exe`
- Address or range: `0x004AADF4`, `0x00466EA0`, `0x00466EA4`, `0x004AB108`
- Summary: Runtime slot state is tracked in a six-entry table rooted at `0x004AADF4`; the primary and secondary selected slot indices are `0x00466EA0` and `0x00466EA4`; the per-slot object array is rooted at `0x004AB108` with stride `0x388`.
- Evidence: `0x0042AA10` clears six entries at `0x004AADF4` and seeds selectors `0x00466EA0=0`, `0x00466EA4=1`; `0x0042B580` iterates the table in six four-byte steps and repeatedly indexes objects under `0x004AB108`; the shared address math resolves to stride `0x388`, and `0x004AB310` is the same object root at offset `0x208`.
- Related session: `analysis-artifacts/sessions/2026-03-17-runtime-slot-table.md`
- Notes: Slot-byte value `3` is the disabled/unavailable state. Slot-byte value `1` is the active runtime-update state consumed by `0x00402E60`.

### Fixed-point angle helper used by the runtime slot controller

- Binary: `TD5_d3d.exe`
- Address or range: `0x0040A720`
- Summary: `0x0040A720` is a quadrant-aware vector-to-angle helper that returns a `0x000..0xFFF` heading from two signed components.
- Evidence: the function branches across all sign quadrants, indexes lookup table `0x00463214`, and returns offset-adjusted values such as `0x400`, `0x800`, `0xC00`, and `0x1000`; `0x00402E60` feeds it position deltas from object fields `+0x1CC` and `+0x1D4` before comparing the result against object `+0x1F4`.
- Related session: `analysis-artifacts/sessions/2026-03-17-runtime-slot-table.md`
- Notes: Working name `AngleFromVector12`.
