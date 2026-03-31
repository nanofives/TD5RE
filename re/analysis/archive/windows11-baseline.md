# Test Drive 5 Windows 11 Baseline

## Current Runtime Split
- `TD5_d3d.exe` loads `M2DX.dll` and is the main DirectDraw/Direct3D path.
- `VOODOO2.exe` loads `M2DXFX.dll` and `glide3x.dll`, so it is the 3dfx/Glide path.
- `Settings.exe`, `TD5.ini`, and `settings.txt` expose legacy options such as `Window`, `NoTripleBuffer`, `NoWBuffer`, `NoMovie`, `NoAGP`, and multi-monitor / secondary-card behavior.

## What To Modernize First
- Prefer the Direct3D path first because it is easier to wrap on Windows 11 than native Glide.
- Treat `M2DX.dll` as the primary engine target for hooks and reverse engineering because it imports `DDRAW.dll`, `DINPUT.dll`, `DSOUND.dll`, and `WINMM.dll`.
- Keep the original binaries untouched. Add wrappers, companion DLLs, launch scripts, and external tooling around them until behavior is mapped.

## Compatibility Strategy
1. Verify baseline launch using `TD5_d3d.exe` with `NoMovie` and windowed/fullscreen variants.
2. Add a DirectDraw/Direct3D wrapper if rendering or presentation breaks on Windows 11.
3. Add Glide translation only for the `VOODOO2.exe` branch if that runtime is still needed.
4. Add instrumentation before patching so crashes and renderer decisions are visible.

## Immediate Hook Targets
- Window creation and display mode selection.
- Timing / frame pacing.
- Input device enumeration and remapping.
- Race-state logging for later gameplay expansion.
- Camera and HUD overlays once rendering flow is mapped.
