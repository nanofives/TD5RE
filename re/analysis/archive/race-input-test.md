# Test Drive 5 Race/Input Test

## Current Baseline
- Runtime path: `TD5_d3d.exe` with `dgVoodoo2`
- Output target: `2560x1440`
- Current internal render observed in logs: `1920x1440x16`
- Forced game mode: `FullScreen`
- Movie disabled: `NoMovie`

## What To Test
1. Launch reaches the main menu without auto-closing.
2. Menu navigation works with keyboard.
3. Enter a quick race.
4. Race scene loads fully.
5. Steering, throttle, brake, and camera controls respond.
6. Pause menu opens and exits correctly.
7. Exit back to menu and then to desktop cleanly.

## What To Watch For
- Black screen after menu selection
- Missing textures or white geometry
- Corrupted triangles / depth issues
- Mouse capture problems
- Input lag or stuck controls
- Crash when entering a race
- Crash when exiting a race

## If A Problem Happens
- Note the exact step where it fails.
- Keep the latest `TD5.log`.
- Keep the latest `TD5_d3d.exe.ddraw-proxy.log` if the proxy is in use.
- Run `tools\collect-td5-diagnostics.ps1` immediately after the failure.

## Useful Settings Already Enabled
- `NoTripleBuffer`
- `NoWBuffer`
- `NoMovie`
- `FixMovie`
- `Log`
