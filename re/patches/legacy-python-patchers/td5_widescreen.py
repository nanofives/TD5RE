"""
TD5 Widescreen Patcher
Patches TD5_d3d.exe to run at a custom resolution.

Usage:
  python td5_widescreen.py              -- reads resolution from td5patch.ini
  python td5_widescreen.py 1920 1080    -- explicit resolution
  python td5_widescreen.py --restore    -- restore from backup

Patch sites (all offsets = VA - 0x00400000):
  0x030AAC  WinMain: MOV [app+0xbc], width_int      (DirectX init width)
  0x030ABC  WinMain: MOV [app+0xc0], height_int     (DirectX init height)
  0x02A998  FUN_0042a950: PUSH height_int            (viewport setup param)
  0x02A99D  FUN_0042a950: PUSH width_int             (viewport setup param)
  0x02A9A7  FUN_0042a950: MOV [DAT_004aaf08], width  (global float width)
  0x02A9B1  FUN_0042a950: MOV [DAT_004aaf0c], height (global float height)

Note: FUN_0042aa10 (race init) reads app+0xbc/0xc0 dynamically,
so patching WinMain is sufficient for race rendering as well.
"""

import struct
import shutil
import sys
import os
import configparser

EXE_PATH    = "D:/Descargas/Test Drive 5 ISO/TD5_d3d.exe"
BACKUP_PATH = EXE_PATH + ".bak"
INI_PATH    = os.path.join(os.path.dirname(__file__), "td5patch.ini")

# --- Original values (640x480) ---
ORIG_WIDTH  = 640
ORIG_HEIGHT = 480

# Patch descriptors: (file_offset, length, description, patcher_fn)
# patcher_fn(w, h) -> bytes to write
def _patch_winmain_width(w, h):
    # C7 81 BC 00 00 00 [width_int_LE:4]
    return b'\xC7\x81\xBC\x00\x00\x00' + struct.pack('<I', w)

def _patch_winmain_height(w, h):
    # C7 82 C0 00 00 00 [height_int_LE:4]
    return b'\xC7\x82\xC0\x00\x00\x00' + struct.pack('<I', h)

def _patch_push_height(w, h):
    # 68 [height_int_LE:4]
    return b'\x68' + struct.pack('<I', h)

def _patch_push_width(w, h):
    # 68 [width_int_LE:4]
    return b'\x68' + struct.pack('<I', w)

def _patch_float_width(w, h):
    # C7 05 08 AF 4A 00 [width_float_LE:4]
    return b'\xC7\x05\x08\xAF\x4A\x00' + struct.pack('<f', float(w))

def _patch_float_height(w, h):
    # C7 05 0C AF 4A 00 [height_float_LE:4]
    return b'\xC7\x05\x0C\xAF\x4A\x00' + struct.pack('<f', float(h))

def _patch_nop_je_race_end(w, h):
    # FUN_00442170 state-2: JE 0x15 -> JMP 0x15  (skip FullScreen on race end)
    return b'\xEB\x15'

def _patch_nop_je_race_init(w, h):
    # FUN_0042aa10: JE 0x1B -> JMP 0x1B  (skip FullScreen on race start)
    return b'\xEB\x1B'

PATCHES = [
    (0x030AAC, 10, "WinMain width int",                 _patch_winmain_width),
    (0x030ABC, 10, "WinMain height int",                _patch_winmain_height),
    (0x02A998,  5, "PUSH height (viewport)",            _patch_push_height),
    (0x02A99D,  5, "PUSH width  (viewport)",            _patch_push_width),
    (0x02A9A7, 10, "MOV float width global",            _patch_float_width),
    (0x02A9B1, 10, "MOV float height global",           _patch_float_height),
    (0x0422B1,  2, "Skip FullScreen on race end",       _patch_nop_je_race_end),
    (0x02B48A,  2, "Skip FullScreen on race start",     _patch_nop_je_race_init),
]

def read_config():
    cfg = configparser.ConfigParser()
    cfg.read(INI_PATH)
    w = cfg.getint("widescreen", "width",  fallback=ORIG_WIDTH)
    h = cfg.getint("widescreen", "height", fallback=ORIG_HEIGHT)
    return w, h

def verify_original(data):
    """Return True if the EXE looks unpatched (contains original 640x480 values)."""
    checks = [
        (0x030AAC, b'\xC7\x81\xBC\x00\x00\x00\x80\x02\x00\x00'),
        (0x02A9A7, b'\xC7\x05\x08\xAF\x4A\x00\x00\x00\x20\x44'),
    ]
    return all(data[off:off+len(b)] == b for off, b in checks)

def verify_current(data, w, h):
    """Return True if the EXE is already patched to w x h."""
    width_bytes  = struct.pack('<I', w)
    float_w      = struct.pack('<f', float(w))
    check_off    = 0x030AAC
    check_bytes  = b'\xC7\x81\xBC\x00\x00\x00' + width_bytes
    return data[check_off:check_off+10] == check_bytes

def apply_patch(w, h):
    if not os.path.exists(EXE_PATH):
        print(f"ERROR: {EXE_PATH} not found.")
        return False

    with open(EXE_PATH, 'rb') as f:
        data = bytearray(f.read())

    if not os.path.exists(BACKUP_PATH):
        print(f"Creating backup: {BACKUP_PATH}")
        shutil.copy2(EXE_PATH, BACKUP_PATH)
    else:
        print(f"Backup already exists: {BACKUP_PATH}")

    # Verify we're patching a known version
    if not verify_original(data) and not verify_current(data, ORIG_WIDTH, ORIG_HEIGHT):
        print("WARNING: EXE doesn't match expected original bytes. Proceeding anyway.")

    if verify_current(data, w, h):
        print(f"EXE is already patched to {w}x{h}. Nothing to do.")
        return True

    print(f"Patching to {w}x{h}:")
    for offset, length, desc, patcher in PATCHES:
        new_bytes = patcher(w, h)
        old_bytes = bytes(data[offset:offset+length])
        assert len(new_bytes) == length, f"Patch length mismatch for {desc}"
        data[offset:offset+length] = new_bytes
        print(f"  0x{offset:06X} {desc}")
        print(f"    before: {' '.join(f'{b:02X}' for b in old_bytes)}")
        print(f"    after:  {' '.join(f'{b:02X}' for b in new_bytes)}")

    with open(EXE_PATH, 'wb') as f:
        f.write(data)

    print(f"\nDone. Patched {EXE_PATH} to {w}x{h}.")
    print("NOTE: HUD/UI elements are not yet adjusted for aspect ratio.")
    return True

def restore_backup():
    if not os.path.exists(BACKUP_PATH):
        print(f"No backup found at {BACKUP_PATH}")
        return False
    shutil.copy2(BACKUP_PATH, EXE_PATH)
    print(f"Restored {EXE_PATH} from backup.")
    return True

if __name__ == "__main__":
    if "--restore" in sys.argv:
        restore_backup()
    elif len(sys.argv) == 3:
        try:
            w, h = int(sys.argv[1]), int(sys.argv[2])
        except ValueError:
            print("Usage: td5_widescreen.py <width> <height>")
            sys.exit(1)
        apply_patch(w, h)
    else:
        w, h = read_config()
        print(f"Resolution from td5patch.ini: {w}x{h}")
        apply_patch(w, h)
