# Test Drive 5 Runtime Hooking Notes

## Primary Target
- `[M2DX.dll](../M2DX.dll)` is the main modernization target for the Direct3D path.
- It imports `DDRAW.dll`, `DINPUT.dll`, `DSOUND.dll`, `WINMM.dll`, `USER32.dll`, and `GDI32.dll`.
- `[TD5_d3d.exe](../TD5_d3d.exe)` is the launcher/runtime host, but most renderer/input leverage is likely behind `M2DX.dll`.

## Secondary Target
- `[M2DXFX.dll](../M2DXFX.dll)` and `[VOODOO2.exe](../VOODOO2.exe)` are the Glide/3dfx path.
- Defer this branch until the Direct3D path is stable on Windows 11.

## First Hooking Goals
- Log window creation and display mode selection.
- Log DirectDraw / Direct3D device creation and fallback decisions.
- Log input device enumeration and selected controller paths.
- Log timing / frame pacing behavior around race start.
- Add a lightweight overlay or log sink before attempting gameplay patches.

## Practical Hooking Approaches
- Wrapper DLLs around imported graphics/input APIs.
- Proxy DLL or injected helper targeting `M2DX.dll` call sites.
- Import table patching only after a logging-only path works.
- Avoid direct binary mutation of the original game files until hook points are repeatable.

## Suggested First Implementation
1. Build a logging wrapper around the DirectDraw path used by `M2DX.dll`.
2. Capture window mode, resolution, renderer creation, and error paths.
3. Confirm that `NoMovie`, `Window`, `NoWBuffer`, and `NoTripleBuffer` settings affect the same code path on Windows 11.
4. Only then start experimenting with widescreen, timing, or input remapping behavior.
