"""
patch_widescreen_m2dx.py
========================
Patches M2DX.dll to remove the 4:3 aspect ratio filter and update default
display mode preferences, enabling widescreen resolutions in TD5.

This is the DLL-side companion to td5_widescreen.py (EXE-side) and
patch_menu_widescreen.py (EXE-side scale caves).

Usage:
  python patch_widescreen_m2dx.py                   -- apply with defaults from td5patch.ini
  python patch_widescreen_m2dx.py 1920 1080          -- explicit target resolution
  python patch_widescreen_m2dx.py --restore           -- restore from backup
  python patch_widescreen_m2dx.py --verify            -- check current patch state
  python patch_widescreen_m2dx.py --dry-run           -- show what would change
  python patch_widescreen_m2dx.py --dry-run 1920 1080 -- dry-run with explicit resolution

Patch sites in M2DX.dll (all file offsets = VA - 0x10000000):
=============================================================

  PATCH 1 -- Remove 4:3 aspect ratio filter
    File offset:  0x80B3
    VA:           0x100080B3
    Function:     RecordDisplayModeIfUsable (EnumDisplayModes callback)
    Original:     74 08           (JZ +8 -- accept only 4:3 modes)
    Patched:      EB 08           (JMP +8 -- accept ALL aspect ratios)
    Effect:       Widescreen modes (16:9, 16:10, 21:9, etc.) appear in the
                  display mode table and become selectable.

  PATCH 2 -- Default fullscreen width in EnumerateDisplayModes
    File offset:  0x7F94
    VA:           0x10007F93 (MOV EBX, imm32)
    Function:     EnumerateDisplayModes
    Original:     BB 80 02 00 00  (MOV EBX, 0x280 = 640)
    Patched:      BB XX XX XX XX  (MOV EBX, target_width)
    Effect:       The engine's preferred fullscreen mode width changes from
                  640 to the target width. If no exact match exists, the engine
                  falls back to the windowed default (desktop resolution).

  PATCH 3 -- Default fullscreen height in EnumerateDisplayModes
    File offset:  0x7FA3
    VA:           0x10007FA2 (CMP dword ptr [EAX], imm32)
    Function:     EnumerateDisplayModes
    Original:     81 38 E0 01 00 00  (CMP [EAX], 0x1E0 = 480)
    Patched:      81 38 XX XX XX XX  (CMP [EAX], target_height)
    Effect:       Companion to Patch 2 -- matches target height for the
                  default fullscreen mode selection.

  PATCH 4 -- Preferred mode width in SelectPreferredDisplayMode
    File offset:  0x2BBD
    VA:           0x10002BBD
    Function:     SelectPreferredDisplayMode
    Original:     81 7E F8 80 02 00 00  (CMP [ESI-8], 0x280 = 640)
    Patched:      81 7E F8 XX XX XX XX  (CMP [ESI-8], target_width)
    Effect:       When the engine searches for a preferred display mode
                  (e.g. after driver change), it looks for target_width
                  instead of 640.

  PATCH 5 -- Preferred mode height in SelectPreferredDisplayMode
    File offset:  0x2BC6
    VA:           0x10002BC6
    Function:     SelectPreferredDisplayMode
    Original:     81 7E FC E0 01 00 00  (CMP [ESI-4], 0x1E0 = 480)
    Patched:      81 7E FC XX XX XX XX  (CMP [ESI-4], target_height)
    Effect:       Companion to Patch 4 -- matches target height.

Architecture notes:
  - M2DX.dll image base: 0x10000000
  - .text section: VA 0x10001000, file offset 0x1000 (alignment matches)
  - File offset = VA - ImageBase for all sections
  - RecordDisplayModeIfUsable is the IDirectDraw4::EnumDisplayModes callback
  - The 4:3 check is: width*3 != height*4 -> reject mode
  - After patching, the mode table accepts all aspect ratios >= 15bpp
  - The sort order (bitdepth desc, width desc, height desc) is unchanged
  - Max 50 mode entries (0x32) -- may need increasing for systems with
    many unique resolutions, but 50 is usually sufficient

Dependencies:
  - td5_widescreen.py should be applied to TD5_d3d.exe for the EXE-side
    resolution globals (WinMain width/height, gRenderWidth/gRenderHeight)
  - patch_menu_widescreen.py should be applied for frontend scale caves
  - The projection focal constant (0.5625 at EXE file offset 0x5D78C) is
    an EXE-side patch handled by td5_widescreen.py or separately
"""

import struct
import shutil
import sys
import os
import configparser

# --- Configuration ---
DLL_PATH    = "D:/Descargas/Test Drive 5 ISO/M2DX.dll"
BACKUP_PATH = DLL_PATH + ".bak"
INI_PATH    = os.path.join(os.path.dirname(__file__), "legacy-python-patchers", "td5patch.ini")

IMAGE_BASE = 0x10000000

# Original resolution
ORIG_WIDTH  = 640
ORIG_HEIGHT = 480

# --- Patch descriptors ---
# (file_offset, original_bytes, description, patch_fn)
# patch_fn(width, height) -> new bytes (must be same length as original)

def _patch_aspect_filter(w, h):
    """Patch 1: JZ -> JMP at the 4:3 aspect ratio check."""
    return bytes([0xEB, 0x08])

def _patch_enum_default_width(w, h):
    """Patch 2: MOV EBX, target_width in EnumerateDisplayModes."""
    return b'\xBB' + struct.pack('<I', w)

def _patch_enum_default_height(w, h):
    """Patch 3: CMP [EAX], target_height in EnumerateDisplayModes."""
    return b'\x81\x38' + struct.pack('<I', h)

def _patch_preferred_width(w, h):
    """Patch 4: CMP [ESI-8], target_width in SelectPreferredDisplayMode."""
    return b'\x81\x7E\xF8' + struct.pack('<I', w)

def _patch_preferred_height(w, h):
    """Patch 5: CMP [ESI-4], target_height in SelectPreferredDisplayMode."""
    return b'\x81\x7E\xFC' + struct.pack('<I', h)


PATCHES = [
    # (file_offset, length, original_bytes, description, patch_fn)
    (
        0x80B3, 2,
        bytes([0x74, 0x08]),
        "Patch 1: 4:3 aspect ratio filter (JZ -> JMP)",
        _patch_aspect_filter,
    ),
    (
        0x7F93, 5,
        bytes([0xBB, 0x80, 0x02, 0x00, 0x00]),
        "Patch 2: EnumerateDisplayModes default width (MOV EBX, 640)",
        _patch_enum_default_width,
    ),
    (
        0x7FA2, 6,
        bytes([0x81, 0x38, 0xE0, 0x01, 0x00, 0x00]),
        "Patch 3: EnumerateDisplayModes default height (CMP [EAX], 480)",
        _patch_enum_default_height,
    ),
    (
        0x2BBD, 7,
        bytes([0x81, 0x7E, 0xF8, 0x80, 0x02, 0x00, 0x00]),
        "Patch 4: SelectPreferredDisplayMode width (CMP [ESI-8], 640)",
        _patch_preferred_width,
    ),
    (
        0x2BC6, 7,
        bytes([0x81, 0x7E, 0xFC, 0xE0, 0x01, 0x00, 0x00]),
        "Patch 5: SelectPreferredDisplayMode height (CMP [ESI-4], 480)",
        _patch_preferred_height,
    ),
]


def read_config():
    """Read target resolution from td5patch.ini."""
    cfg = configparser.ConfigParser()
    cfg.read(INI_PATH)
    w = cfg.getint("widescreen", "width",  fallback=ORIG_WIDTH)
    h = cfg.getint("widescreen", "height", fallback=ORIG_HEIGHT)
    return w, h


def check_state(data):
    """
    Determine the current patch state.
    Returns: ('original', None) | ('patched', (w, h)) | ('unknown', None)
    """
    # Check if aspect filter is original
    aspect_off = 0x80B3
    if data[aspect_off] == 0x74 and data[aspect_off + 1] == 0x08:
        # Check enum default width is original 640
        enum_w_off = 0x7F94  # immediate starts 1 byte after opcode
        w_bytes = data[enum_w_off:enum_w_off + 4]
        if w_bytes == struct.pack('<I', ORIG_WIDTH):
            return ('original', None)
        else:
            return ('unknown', None)

    elif data[aspect_off] == 0xEB and data[aspect_off + 1] == 0x08:
        # Patched -- read the target resolution from enum default
        enum_w_off = 0x7F94
        enum_h_off = 0x7FA4  # immediate starts 2 bytes after opcode
        w = struct.unpack('<I', data[enum_w_off:enum_w_off + 4])[0]
        h = struct.unpack('<I', data[enum_h_off:enum_h_off + 4])[0]
        return ('patched', (w, h))

    else:
        return ('unknown', None)


def verify_cmd(dll_path):
    """Print the current patch state."""
    if not os.path.exists(dll_path):
        print(f"ERROR: {dll_path} not found.")
        return False

    with open(dll_path, 'rb') as f:
        data = f.read()

    state, info = check_state(bytearray(data))
    if state == 'original':
        print(f"M2DX.dll is UNPATCHED (original 640x480, 4:3 filter active).")
    elif state == 'patched':
        print(f"M2DX.dll is PATCHED for {info[0]}x{info[1]} (4:3 filter removed).")
    else:
        print(f"M2DX.dll is in an UNKNOWN state (may be partially patched).")

    print(f"\nDetailed byte check:")
    for offset, length, orig, desc, _ in PATCHES:
        actual = data[offset:offset + length]
        va = IMAGE_BASE + offset
        is_orig = (actual == orig)
        print(f"  0x{offset:05X} (VA 0x{va:08X}): {' '.join(f'{b:02X}' for b in actual)}"
              f"  {'[ORIGINAL]' if is_orig else '[MODIFIED]'}  {desc}")

    return True


def apply_patch(w, h, dll_path, dry_run=False):
    """Apply all widescreen patches to M2DX.dll."""
    if not os.path.exists(dll_path):
        print(f"ERROR: {dll_path} not found.")
        return False

    with open(dll_path, 'rb') as f:
        data = bytearray(f.read())

    state, info = check_state(data)

    if state == 'patched' and info == (w, h):
        print(f"M2DX.dll is already patched for {w}x{h}. Nothing to do.")
        return True

    if state == 'patched' and info != (w, h):
        print(f"M2DX.dll is currently patched for {info[0]}x{info[1]}, "
              f"re-patching to {w}x{h}.")

    if not dry_run:
        backup_path = dll_path + ".bak"
        if not os.path.exists(backup_path):
            print(f"Creating backup: {backup_path}")
            shutil.copy2(dll_path, backup_path)
        else:
            print(f"Backup already exists: {backup_path}")

    prefix = "[DRY RUN] " if dry_run else ""
    print(f"\n{prefix}Patching M2DX.dll for {w}x{h}:")
    print(f"{'=' * 60}")

    for offset, length, orig, desc, patch_fn in PATCHES:
        new_bytes = patch_fn(w, h)
        old_bytes = bytes(data[offset:offset + length])
        va = IMAGE_BASE + offset
        assert len(new_bytes) == length, \
            f"Patch length mismatch for {desc}: expected {length}, got {len(new_bytes)}"

        changed = old_bytes != new_bytes
        status = "CHANGE" if changed else "same"

        print(f"\n  {desc}")
        print(f"    File offset: 0x{offset:05X}  VA: 0x{va:08X}")
        print(f"    Before: {' '.join(f'{b:02X}' for b in old_bytes)}")
        print(f"    After:  {' '.join(f'{b:02X}' for b in new_bytes)}  [{status}]")

        if not dry_run:
            data[offset:offset + length] = new_bytes

    if not dry_run:
        with open(dll_path, 'wb') as f:
            f.write(data)
        print(f"\n{'=' * 60}")
        print(f"Done. Patched {dll_path} for {w}x{h}.")
    else:
        print(f"\n{'=' * 60}")
        print(f"Dry run complete. No files modified.")

    print(f"\nReminder: also apply EXE-side patches with:")
    print(f"  python td5_widescreen.py {w} {h}")
    print(f"  python patch_menu_widescreen.py")

    return True


def restore_backup(dll_path):
    """Restore M2DX.dll from backup."""
    backup_path = dll_path + ".bak"
    if not os.path.exists(backup_path):
        print(f"No backup found at {backup_path}")
        return False
    shutil.copy2(backup_path, dll_path)
    print(f"Restored {dll_path} from backup.")
    return True


def main():
    dll_path = DLL_PATH

    # Allow DLL_PATH override via environment
    if "M2DX_DLL_PATH" in os.environ:
        dll_path = os.environ["M2DX_DLL_PATH"]

    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = [a for a in sys.argv[1:] if a.startswith("--")]

    if "--restore" in flags:
        restore_backup(dll_path)
        return

    if "--verify" in flags:
        verify_cmd(dll_path)
        return

    dry_run = "--dry-run" in flags

    if len(args) == 2:
        try:
            w, h = int(args[0]), int(args[1])
        except ValueError:
            print("Usage: patch_widescreen_m2dx.py [--dry-run] [<width> <height>]")
            print("       patch_widescreen_m2dx.py --restore")
            print("       patch_widescreen_m2dx.py --verify")
            sys.exit(1)
    elif len(args) == 0:
        w, h = read_config()
        if w == ORIG_WIDTH and h == ORIG_HEIGHT:
            print(f"No custom resolution in td5patch.ini, using 640x480.")
            print(f"Specify resolution: patch_widescreen_m2dx.py <width> <height>")
        else:
            print(f"Resolution from td5patch.ini: {w}x{h}")
    else:
        print("Usage: patch_widescreen_m2dx.py [--dry-run] [<width> <height>]")
        sys.exit(1)

    apply_patch(w, h, dll_path, dry_run=dry_run)


if __name__ == "__main__":
    main()
