# Test Drive 5 — Windowed-Mode Recipe

**Status:** working on Windows 11 as of 2026-04-20. Produces a real movable
640×480 window, desktop resolution preserved, correct colors in frontend
and race, no screen flicker.

This is the canonical recipe for running the original unmodified
`TD5_d3d.exe` in windowed mode. Both parts (`asi_patcher` + `td5_ddraw`)
must be installed; each solves a different layer of the problem. The
on-disk `TD5_d3d.exe` and `M2DX.dll` stay **pristine** — no `.bak` files
required.

## What the original problem was

1. TD5 was built for 1999-era DirectDraw: it requests `DDSCL_EXCLUSIVE |
   DDSCL_FULLSCREEN` and a 16bpp primary surface, and expects DirectDraw
   to mode-switch the desktop to match.
2. On modern Windows the Program Compatibility Assistant auto-assigns the
   `DWM8And16BitMitigation` shim to any legacy DDraw game. That shim takes
   the kernel-level `D3DKMTSetDisplayMode` path to force the desktop to
   640×480 whenever the game creates its primary, which is exactly what
   we don't want.
3. Even when DDSCL_NORMAL is forced, DirectDraw silently creates offscreen
   surfaces in the *desktop* pixel format (32bpp) if the game doesn't
   specify one — but the game writes 16bpp bytes into them, producing
   garbled menu colors.
4. The window M2DX creates is `WS_POPUP` at the render size, so the outer
   640×480 leaves no room for the caption and the bottom of the picture is
   clipped.

## The fix, in two parts

### Part A — `asi_patcher/` (byte patches at runtime)

`td5_windowed.asi` is an Ultimate-ASI-Loader-style plugin loaded by a
`winmm.dll` proxy. On `DLL_PROCESS_ATTACH` it applies 3 bytes of edits to
`TD5_d3d.exe` and 5 bytes of edits to `M2DX.dll` in memory only. The
patches:

* Skip the game's explicit `DXD3D::FullScreen` calls (two sites in the
  EXE) so the game doesn't fight us every frame.
* Rewrite M2DX's `if (fullscreen) … else normal` jumps so every cooperative
  level call takes the normal path.
* Zero the `Environment` "fullscreen preferred" data-segment flag.
* Change the `CreateWindowExA` dwStyle from `WS_POPUP` to
  `WS_OVERLAPPEDWINDOW` so the window has a caption, borders, and can be
  dragged/closed.
* Patch one crash path in `ResetTexturePageCacheState` that the original
  on-disk recipe also neuters.

### Part B — `td5_ddraw/` (DirectDraw proxy)

`ddraw.dll` in the game folder is our proxy; `ddraw_real.dll` is a plain
copy of the system DirectDraw that we forward unhooked exports to. The
proxy:

* Implements `DirectDrawCreate` itself and returns a wrapped
  `IDirectDraw`. QueryInterface chains produce wrapped `IDirectDraw4` and
  `IDirectDraw7` on demand. All unrelated methods delegate to the real
  interface.
* Overrides `SetCooperativeLevel` — flags are force-rewritten to
  `DDSCL_NORMAL` before the real call, so Windows never enters exclusive
  fullscreen and the shim engine has nothing to trigger on.
* Overrides `SetDisplayMode` — returns `DD_OK` without touching the
  desktop.
* Wraps `CreateSurface` with two fixes:
  - If the requested surface has no `DDSD_PIXELFORMAT` flag and isn't the
    primary/Z-buffer/palette, inject 16bpp RGB565 before passing through.
    This keeps menu art surfaces at 16bpp so the game's 16bpp writes land
    correctly. (The textures Game creates for D3D already specify their
    own format, so race mode was never affected.)
  - After creating the primary, call `AdjustWindowRectEx` +
    `SetWindowPos` on the game's HWND so the **client** area is exactly
    the requested render size (640×480). This compensates for M2DX's
    broken window resize on modern DPI behaviour.

## Directory layout

```
TD5RE/
├── original/                      Game files (pristine binaries)
│   ├── TD5_d3d.exe                unmodified
│   ├── M2DX.dll                   unmodified
│   ├── ddraw.dll                  ← td5_ddraw proxy (installed)
│   ├── ddraw_real.dll             ← copy of C:\Windows\SysWOW64\ddraw.dll
│   ├── winmm.dll                  ← asi_patcher winmm proxy (installed)
│   ├── winmm_real.dll             ← copy of C:\Windows\SysWOW64\winmm.dll
│   └── td5_windowed.asi           ← asi_patcher byte-patch plugin
├── asi_patcher/                   Sources + build scripts for the .asi
├── td5_ddraw/                     Sources + build scripts for the proxy
└── td5mod/deps/mingw/             Bundled MinGW-w64 i686 toolchain
```

## Recreate from scratch (if `original/` or this install is lost)

All commands run from `TD5RE/`. Needs MinGW i686 (bundled), Python (any
3.x), and a reference copy of `original/TD5_d3d.exe` + `M2DX.dll` +
support files.

```batch
REM 1. Generate forwarder .def files from system DLLs.
python asi_patcher\generate_winmm_def.py
python td5_ddraw\generate_def.py

REM 2. Build both components.
asi_patcher\build.bat
td5_ddraw\build.bat

REM 3. Install into original\ (copies DLLs + .asi, copies system
REM    winmm/ddraw as *_real.dll alongside).
asi_patcher\install.bat
td5_ddraw\install.bat
```

## Launching

Launch via `asi_patcher\run_no_shims.ps1`. It sets
`__COMPAT_LAYER=RunAsInvoker` in the child environment, which prevents
the Windows AppCompat shim engine from injecting `DWM8And16BitMitigation`
on this run.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass ^
    -File asi_patcher\run_no_shims.ps1
```

Do **not** double-click `TD5_d3d.exe` directly unless you've cleared the
HKCU/HKLM AppCompat Layer entries for its path. The PCA engine
re-populates them after every regular run, so the robust answer is to
always launch through the script above.

## One-time environment setup

Windows PCA auto-adds compat shims on first launch and every subsequent
run. They are harmless when the process is launched with
`__COMPAT_LAYER=RunAsInvoker` (the shim engine honours the override), but
if you ever launch without it and the shims take effect, clear them:

```batch
asi_patcher\clean_shims.cmd                  REM HKCU (per-user)
asi_patcher\remove_hklm_shim_ADMIN.cmd       REM HKLM (run as admin)
```

`check_shims.cmd` prints the current state at any time.

## Known-good verification log

When everything is working, `original\td5_ddraw.log` starts with:

```
[proxy] td5_ddraw.dll attached
[proxy] real ddraw loaded @ <addr>
[proxy] DirectDrawCreate guid=<…> -> hr=0x00000000 real=<ptr>
…
[dd4] SetCooperativeLevel(hwnd=<hwnd> flags=0x00000008) forced DDSCL_NORMAL
[proxy] forced client size 640x480 (window <outer>x<outer>)
```

and `original\td5_windowed.log` starts with:

```
td5_windowed.asi attached (EXE base=00400000)
[TD5_d3d.exe] +0x2B48A patched: …
[TD5_d3d.exe] +0x422B1 patched: …
[TD5_d3d.exe] +0x0BA60 patched: …
[M2DX.dll]    +0x06637 patched: …
[M2DX.dll]    +0x078CA patched: …
[M2DX.dll]    +0x06684 patched: …
[M2DX.dll]    +0x124C5 patched: …
[M2DX.dll]    +0x12ADB patched: …
```

If either log is missing, something is blocking the proxy from loading;
by far the most common cause is `WINXPSP2`/`WINXPSP3` in the AppCompat
layer for the EXE (it breaks DLL redirection). Run the launcher through
`run_no_shims.ps1` or clear shims.

## What NOT to use anymore (pre-2026-04-20 recipes, now superseded)

* DDrawCompat as `ddraw.dll` — gave borderless fullscreen, not true
  windowed. Replaced by `td5_ddraw`.
* `patch_windowed_td5exe.py` / `patch_windowed_m2dx.py` against on-disk
  binaries — replaced by the `.asi` runtime patcher. Keep the Python
  scripts as byte-level documentation of what each patch does.
* `~ 16BITCOLOR WINXPSP3` compat shim — used to work April-10, stopped
  working April-18. The `td5_ddraw` proxy makes it unnecessary.
* The previous `ddraw_proxy/` directory (inline-detour DDraw proxy
  experiment) — deleted 2026-04-20; its ideas live on in `td5_ddraw`.
