"""
patch_windowed_td5exe.py
========================
EXE-side companion to patch_windowed_m2dx.py. Skips the two call sites
where TD5_d3d.exe asks M2DX.dll to switch to exclusive fullscreen during
race bring-up. With these two branches neutered, the engine stays in the
DDSCL_NORMAL state set up at startup and the race path does not tear down
surfaces via the M2DX fullscreen error-cleanup path, which is what was
corrupting the game heap before Reset/LoadRaceTexturePages ran.

Both patch sites are identical-looking `if (DX::app.isFullscreenRequested)
DXD3D::FullScreen(...)` guards. The JZ-around-the-call is flipped to an
unconditional JMP so the call is always skipped.

  PATCH A -- InitializeRaceSession
    File offset:  0x2B48A  VA: 0x0042B48A
    Function:     InitializeRaceSession (0x0042AA10)
    Original:     74 1B           (JZ 0x0042B4A7  -- skip FullScreen)
    Patched:      EB 1B           (JMP 0x0042B4A7 -- always skip)
    Effect:       Race bring-up keeps the initial DDSCL_NORMAL coop level
                  and does NOT call DXD3D::FullScreen, which would have
                  called ConfigureDirectDrawCooperativeLevel(hWnd, 1) +
                  ApplyDirectDrawDisplayMode. In windowed mode the
                  SetDisplayMode leg returns DDERR_NOEXCLUSIVEMODE and
                  M2DX's error-cleanup releases every surface (primary,
                  back buffer, Z-buffer, clipper, gamma, render target)
                  and invokes the release-viewport callback, leaving
                  TD5_d3d.exe's texture-cache state dangling. Later on,
                  ResetTexturePageCacheState dereferences a pointer into
                  the freed region and AVs writing to 0x16674000.

  PATCH B -- RunMainGameLoop
    File offset:  0x422B1  VA: 0x004422B1
    Function:     RunMainGameLoop (0x00442170)
    Original:     74 15           (JZ 0x004422C8  -- skip FullScreen)
    Patched:      EB 15           (JMP 0x004422C8 -- always skip)
    Effect:       Same gate pattern in the outer loop where the engine
                  re-enters fullscreen after a pause/menu round-trip. We
                  want the game to remain windowed for the entire life
                  of the process.

Usage:
  python patch_windowed_td5exe.py                -- apply
  python patch_windowed_td5exe.py --restore      -- restore from backup
  python patch_windowed_td5exe.py --verify       -- check state
  python patch_windowed_td5exe.py --dry-run      -- show changes
  python patch_windowed_td5exe.py --exe <path>   -- override target path
"""

import os
import sys
import shutil

IMAGE_BASE = 0x00400000

DEFAULT_EXE = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "..", "original", "TD5_d3d.exe")
)

PATCH_SKIP_FULLSCREEN_RACE_INIT = (
    0x2B48A, 2,
    bytes([0x74, 0x1B]),
    bytes([0xEB, 0x1B]),
    "Patch A: InitializeRaceSession skip DXD3D::FullScreen (JZ -> JMP)",
)
PATCH_SKIP_FULLSCREEN_MAIN_LOOP = (
    0x422B1, 2,
    bytes([0x74, 0x15]),
    bytes([0xEB, 0x15]),
    "Patch B: RunMainGameLoop skip DXD3D::FullScreen (JZ -> JMP)",
)
PATCH_NEUTER_RESET_TEX_CACHE = (
    0xBA60, 1,
    bytes([0xA1]),
    bytes([0xC3]),
    "Patch C: ResetTexturePageCacheState -> RET (skip corrupt-pointer loop)",
)

ALL_PATCHES = [
    PATCH_SKIP_FULLSCREEN_RACE_INIT,
    PATCH_SKIP_FULLSCREEN_MAIN_LOOP,
    PATCH_NEUTER_RESET_TEX_CACHE,
]


def check_state(data, patches):
    np = no = 0
    for off, length, orig, new, _ in patches:
        actual = bytes(data[off:off + length])
        if actual == new: np += 1
        elif actual == orig: no += 1
    if np == len(patches): return "patched"
    if no == len(patches): return "original"
    if np + no == len(patches): return "partial"
    return "unknown"


def print_state(data, patches):
    for off, length, orig, new, desc in patches:
        va = IMAGE_BASE + off
        actual = bytes(data[off:off + length])
        tag = "[PATCHED] " if actual == new else "[ORIGINAL]" if actual == orig else "[UNKNOWN] "
        h = " ".join(f"{b:02X}" for b in actual)
        print(f"  0x{off:05X} (VA 0x{va:08X}): {h:<5}  {tag}  {desc}")


def verify(exe_path):
    if not os.path.exists(exe_path):
        print(f"ERROR: {exe_path} not found.")
        return
    with open(exe_path, "rb") as f:
        data = f.read()
    state = check_state(data, ALL_PATCHES)
    label = {
        "original": "UNPATCHED (FullScreen calls active)",
        "patched":  "PATCHED (FullScreen calls skipped)",
        "partial":  "PARTIAL (some sites patched)",
        "unknown":  "UNKNOWN (bytes mismatch)",
    }[state]
    print(f"TD5_d3d.exe state: {label}\n")
    print_state(data, ALL_PATCHES)


def apply_patch(exe_path, dry_run=False):
    if not os.path.exists(exe_path):
        print(f"ERROR: {exe_path} not found.")
        return
    with open(exe_path, "rb") as f:
        data = bytearray(f.read())
    state = check_state(data, ALL_PATCHES)
    if state == "patched":
        print("TD5_d3d.exe is already patched. Nothing to do.")
        return
    if state == "unknown":
        print("WARNING: unexpected bytes at patch sites; aborting. Run --verify.")
        return

    if not dry_run:
        backup = exe_path + ".bak"
        if not os.path.exists(backup):
            print(f"Creating backup: {backup}")
            shutil.copy2(exe_path, backup)
        else:
            print(f"Backup already exists: {backup}")

    prefix = "[DRY RUN] " if dry_run else ""
    print(f"\n{prefix}Applying TD5_d3d.exe patches:")
    print("=" * 72)
    for off, length, orig, new, desc in ALL_PATCHES:
        va = IMAGE_BASE + off
        old = bytes(data[off:off + length])
        status = "CHANGE" if old != new else "same"
        print(f"\n  {desc}")
        print(f"    File offset: 0x{off:05X}  VA: 0x{va:08X}")
        print(f"    Before: {' '.join(f'{b:02X}' for b in old)}")
        print(f"    After:  {' '.join(f'{b:02X}' for b in new)}  [{status}]")
        if not dry_run:
            data[off:off + length] = new

    if not dry_run:
        with open(exe_path, "wb") as f:
            f.write(data)
        print("\n" + "=" * 72)
        print(f"Done. Patched {exe_path}.")
    else:
        print("\n" + "=" * 72)
        print("Dry run complete.")


def restore(exe_path):
    backup = exe_path + ".bak"
    if not os.path.exists(backup):
        print(f"No backup found at {backup}")
        return
    shutil.copy2(backup, exe_path)
    print(f"Restored {exe_path} from backup.")


def main():
    args = sys.argv[1:]
    exe_path = DEFAULT_EXE
    if "--exe" in args:
        i = args.index("--exe")
        exe_path = args[i + 1]
        del args[i:i + 2]

    flags = [a for a in args if a.startswith("--")]
    print(f"Target EXE: {exe_path}\n")
    if "--restore" in flags:
        restore(exe_path); return
    if "--verify" in flags:
        verify(exe_path); return
    apply_patch(exe_path, dry_run="--dry-run" in flags)


if __name__ == "__main__":
    main()
