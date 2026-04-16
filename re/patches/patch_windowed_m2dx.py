"""
patch_windowed_m2dx.py
======================
Patches M2DX.dll to force the original Test Drive 5 (TD5_d3d.exe) into
windowed mode — both the frontend and the race engine run inside a regular
window instead of taking over the display.

Three byte patches, all inside M2DX.dll (image base 0x10000000):

  PATCH 1 -- Bypass adapter "no windowed support" check
    File offset:  0x6637   VA: 0x10006637
    Function:     DXDraw::Create (0x100062a0)
    Original:     75 45           (JNZ +0x45)
    Patched:      EB 45           (JMP +0x45)
    Effect:       Skips the branch that sets g_isFullscreenCooperative=1
                  when the enumerated adapter record reports it cannot
                  support DDSCL_NORMAL. With this patch we always fall
                  through to the DDSCL_NORMAL SetCooperativeLevel call.

  PATCH 2 -- Force DDSCL_NORMAL path in ConfigureDirectDrawCooperativeLevel
    File offset:  0x78CA   VA: 0x100078CA
    Function:     ConfigureDirectDrawCooperativeLevel (0x100078A0)
    Original:     74 44           (JZ +0x44)
    Patched:      EB 44           (JMP +0x44)
    Effect:       The `if (param_2 != 0)` test that picks between
                  DDSCL_EXCLUSIVE|DDSCL_FULLSCREEN and DDSCL_NORMAL
                  is unconditionally forced to the DDSCL_NORMAL branch.

  PATCH 2b -- Force windowed setcoop branch in DXDraw::Create
    File offset:  0x6684   VA: 0x10006684
    Function:     DXDraw::Create (0x100062A0)
    Original:     74 68           (JZ 0x100066EE)
    Patched:      EB 68           (JMP 0x100066EE)
    Effect:       After Patch 1 skips the force-fullscreen block, the
                  fall-through check is `CMP g_isFullscreenCooperative, 0;
                  JZ ddscl_normal_path`. If the flag is non-zero at this
                  point (Environment/Create paths can leave it set) the
                  game still falls into the DDSCL_EXCLUSIVE branch and
                  changes the desktop resolution. Turning the JZ into an
                  unconditional JMP guarantees the DDSCL_NORMAL path is
                  taken regardless of the flag's value.

  PATCH 2c -- Clear the "fullscreen preferred" startup flag
    File offset:  0x124C5  VA: 0x100124C5
    Function:     Environment (0x100124B0)
    Original:     01 00 00 00     (MOV [0x10061c1c], 1 immediate)
    Patched:      00 00 00 00     (MOV [0x10061c1c], 0)
    Effect:       InitializeD3DDriverAndMode (0x100034D0) reads
                  DAT_10061c1c and uses it to pick the D3D bring-up
                  branch:
                    != 0 -> ConfigureDirectDrawCooperativeLevel(hWnd, 1)
                            + ApplyDirectDrawDisplayMode (= SetDisplayMode)
                    == 0 -> ConfigureDirectDrawCooperativeLevel(hWnd, 0)
                            and no SetDisplayMode at all.
                  Environment initialises the flag to 1, so without this
                  patch the resolution still changes during D3D bring-up
                  even though DXDraw::Create succeeded with DDSCL_NORMAL.

  PATCH 3 -- Rewrite dwStyle in CreateWindowExA call
    File offset:  0x12ADB  VA: 0x10012ADB
    Function:     DXWin::Initialize (0x10012A10)
    Original:     68 00 00 00 80  (PUSH 0x80000000  -- WS_POPUP)
    Patched:      68 00 00 CF 00  (PUSH 0x00CF0000  -- WS_OVERLAPPEDWINDOW)
    Effect:       Gives the game window a real caption, borders, and the
                  system menu (drag, minimise, close). Without this
                  patch the engine creates a borderless WS_POPUP at the
                  configured render size -- still windowed, but not
                  movable and easy to confuse with a fullscreen cover.

Why these three and only these three:
  - DXWin::Initialize already writes `g_isFullscreenCooperative = 0` at the
    bottom of the function, so no data-segment flag patch is needed.
  - ApplyWindowedRenderSize (0x10008F60) re-adjusts the window rect via
    SetWindowPos after the cooperative level flips, so the WS_OVERLAPPEDWINDOW
    outer size becomes (render_w + frame, render_h + caption + frame) and
    the client area is re-sized to the requested render dimensions.
    The Flip path uses g_clientOriginX/Y + the client rect for its Blt, so
    presents end up correct without any further patches.

Usage:
  python patch_windowed_m2dx.py                 -- apply to default DLL
  python patch_windowed_m2dx.py --restore       -- restore from backup
  python patch_windowed_m2dx.py --verify        -- check current patch state
  python patch_windowed_m2dx.py --dry-run       -- show what would change
  python patch_windowed_m2dx.py --no-caption    -- skip Patch 3 (keep WS_POPUP)
  python patch_windowed_m2dx.py --dll <path>    -- override M2DX.dll location

Dependencies:
  - None -- operates on a fresh or widescreen-patched M2DX.dll.
  - Compatible with patch_widescreen_m2dx.py (different byte ranges).
"""

import os
import sys
import shutil
import struct

IMAGE_BASE = 0x10000000

# Default DLL path = original/ folder in the repo root.
DEFAULT_DLL = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "..", "original", "M2DX.dll")
)

# WS_OVERLAPPEDWINDOW = WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|
#                      WS_THICKFRAME|WS_MINIMIZEBOX|WS_MAXIMIZEBOX
WS_POPUP            = 0x80000000
WS_OVERLAPPEDWINDOW = 0x00CF0000

# (file_offset, length, original_bytes, patched_bytes, description)
PATCH_ADAPTER_JNZ = (
    0x6637, 2,
    bytes([0x75, 0x45]),
    bytes([0xEB, 0x45]),
    "Patch 1: DXDraw::Create adapter windowed check (JNZ -> JMP)",
)
PATCH_COOPLEVEL_JZ = (
    0x78CA, 2,
    bytes([0x74, 0x44]),
    bytes([0xEB, 0x44]),
    "Patch 2: ConfigureDirectDrawCooperativeLevel normal path (JZ -> JMP)",
)
PATCH_CREATE_SETCOOP_JZ = (
    0x6684, 2,
    bytes([0x74, 0x68]),
    bytes([0xEB, 0x68]),
    "Patch 2b: DXDraw::Create force DDSCL_NORMAL branch (JZ -> JMP)",
)
PATCH_ENVIRONMENT_FLAG = (
    0x124C5, 4,
    bytes([0x01, 0x00, 0x00, 0x00]),
    bytes([0x00, 0x00, 0x00, 0x00]),
    "Patch 2c: Environment DAT_10061c1c startup flag (1 -> 0)",
)
PATCH_WINDOW_STYLE = (
    0x12ADB, 5,
    b"\x68" + struct.pack("<I", WS_POPUP),
    b"\x68" + struct.pack("<I", WS_OVERLAPPEDWINDOW),
    "Patch 3: DXWin::Initialize CreateWindowExA dwStyle (WS_POPUP -> WS_OVERLAPPEDWINDOW)",
)

ALL_PATCHES = [
    PATCH_ADAPTER_JNZ,
    PATCH_COOPLEVEL_JZ,
    PATCH_CREATE_SETCOOP_JZ,
    PATCH_ENVIRONMENT_FLAG,
    PATCH_WINDOW_STYLE,
]


def check_state(data, patches):
    """Return 'original' | 'patched' | 'partial' | 'unknown' for the given patch set."""
    matches_patched  = 0
    matches_original = 0
    for offset, length, orig, patched, _desc in patches:
        actual = bytes(data[offset:offset + length])
        if actual == patched:
            matches_patched += 1
        elif actual == orig:
            matches_original += 1
    if matches_patched == len(patches):
        return "patched"
    if matches_original == len(patches):
        return "original"
    if matches_patched + matches_original == len(patches):
        return "partial"
    return "unknown"


def print_state(data, patches):
    for offset, length, orig, patched, desc in patches:
        va = IMAGE_BASE + offset
        actual = bytes(data[offset:offset + length])
        if actual == patched:
            tag = "[PATCHED] "
        elif actual == orig:
            tag = "[ORIGINAL]"
        else:
            tag = "[UNKNOWN] "
        hexbytes = " ".join(f"{b:02X}" for b in actual)
        print(f"  0x{offset:05X} (VA 0x{va:08X}): {hexbytes:<17}  {tag}  {desc}")


def verify_cmd(dll_path, patches):
    if not os.path.exists(dll_path):
        print(f"ERROR: {dll_path} not found.")
        return False
    with open(dll_path, "rb") as f:
        data = f.read()
    state = check_state(data, patches)
    label = {
        "original": "UNPATCHED (original fullscreen-only)",
        "patched":  "PATCHED (windowed mode enabled)",
        "partial":  "PARTIALLY PATCHED (some patches applied)",
        "unknown":  "UNKNOWN (bytes don't match original or patched)",
    }[state]
    print(f"M2DX.dll state: {label}")
    print()
    print_state(data, patches)
    return True


def apply_patch(dll_path, patches, dry_run=False):
    if not os.path.exists(dll_path):
        print(f"ERROR: {dll_path} not found.")
        return False

    with open(dll_path, "rb") as f:
        data = bytearray(f.read())

    state = check_state(data, patches)
    if state == "patched":
        print("M2DX.dll is already patched for windowed mode. Nothing to do.")
        return True
    if state == "unknown":
        print("WARNING: M2DX.dll has unexpected bytes at one or more patch sites.")
        print("         This DLL may already be patched by an unrelated tool.")
        print("         Aborting to avoid corruption. Run --verify for details.")
        return False

    if not dry_run:
        backup_path = dll_path + ".bak"
        if not os.path.exists(backup_path):
            print(f"Creating backup: {backup_path}")
            shutil.copy2(dll_path, backup_path)
        else:
            print(f"Backup already exists: {backup_path}")

    prefix = "[DRY RUN] " if dry_run else ""
    print(f"\n{prefix}Applying windowed-mode patches to M2DX.dll:")
    print("=" * 72)

    for offset, length, orig, patched, desc in patches:
        va = IMAGE_BASE + offset
        old = bytes(data[offset:offset + length])
        will_change = old != patched
        status = "CHANGE" if will_change else "same"

        print()
        print(f"  {desc}")
        print(f"    File offset: 0x{offset:05X}  VA: 0x{va:08X}")
        print(f"    Before: {' '.join(f'{b:02X}' for b in old)}")
        print(f"    After:  {' '.join(f'{b:02X}' for b in patched)}  [{status}]")

        if not dry_run:
            data[offset:offset + length] = patched

    if not dry_run:
        with open(dll_path, "wb") as f:
            f.write(data)
        print("\n" + "=" * 72)
        print(f"Done. Patched {dll_path} for windowed mode.")
        print("Launch TD5_d3d.exe normally; both frontend and race should be windowed.")
    else:
        print("\n" + "=" * 72)
        print("Dry run complete. No files modified.")

    return True


def restore_backup(dll_path):
    backup_path = dll_path + ".bak"
    if not os.path.exists(backup_path):
        print(f"No backup found at {backup_path}")
        return False
    shutil.copy2(backup_path, dll_path)
    print(f"Restored {dll_path} from backup.")
    return True


def main():
    args = sys.argv[1:]

    dll_path = DEFAULT_DLL
    if "--dll" in args:
        i = args.index("--dll")
        dll_path = args[i + 1]
        del args[i:i + 2]

    flags = [a for a in args if a.startswith("--")]
    patches = list(ALL_PATCHES)
    if "--no-caption" in flags:
        patches = [
            PATCH_ADAPTER_JNZ,
            PATCH_COOPLEVEL_JZ,
            PATCH_CREATE_SETCOOP_JZ,
            PATCH_ENVIRONMENT_FLAG,
        ]
        flags.remove("--no-caption")

    print(f"Target DLL: {dll_path}")
    print()

    if "--restore" in flags:
        restore_backup(dll_path)
        return

    if "--verify" in flags:
        verify_cmd(dll_path, patches)
        return

    dry_run = "--dry-run" in flags
    apply_patch(dll_path, patches, dry_run=dry_run)


if __name__ == "__main__":
    main()
