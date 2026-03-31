"""
apply_cwd_patch.py - SetCurrentDirectory("data") trampoline for TD5_d3d.exe

Patches the game to call SetCurrentDirectory("data") at WinMain entry,
redirecting all relative file I/O to the data/ subfolder. DLLs are loaded
by the PE loader before WinMain, so they remain in root unaffected.

Binary modifications (86 bytes total):
  0x45C114 (39 bytes) - String data: "kernel32.dll", "SetCurrentDirectoryA", "data"
  0x45C140 (42 bytes) - Trampoline code
  0x430A90 (5 bytes)  - JMP redirect at WinMain entry

Note: 0x45C110 contains C3 (RET) from the last function. Free space starts at 0x45C111.

Compatible with widescreen scale cave at 0x45C330 (no overlap).

IAT slots used (resolved at runtime by PE loader):
  0x45D0E0 - GetProcAddress
  0x45D108 - GetModuleHandleA

Usage:
  python apply_cwd_patch.py                    # patch TD5_d3d.exe in parent dir
  python apply_cwd_patch.py path/to/TD5_d3d.exe
  python apply_cwd_patch.py --dry-run          # show what would be patched
  python apply_cwd_patch.py --revert           # restore original bytes
"""
import struct
import sys
import os
import shutil

# --- Addresses (virtual) ---
IMAGE_BASE      = 0x00400000
WINMAIN_VA      = 0x00430A90
CAVE_STRINGS_VA = 0x0045C114
CAVE_CODE_VA    = 0x0045C140
IAT_GETPROCADDR = 0x0045D0E0
IAT_GETMODHDL   = 0x0045D108

# --- File offsets (VA - IMAGE_BASE for .text section where file_offset == RVA) ---
def va_to_offset(va):
    """Convert virtual address to file offset. .text RVA == raw offset in this PE."""
    return va - IMAGE_BASE

WINMAIN_OFF      = va_to_offset(WINMAIN_VA)
CAVE_STRINGS_OFF = va_to_offset(CAVE_STRINGS_VA)
CAVE_CODE_OFF    = va_to_offset(CAVE_CODE_VA)

# --- Original bytes at WinMain entry (for verification and revert) ---
WINMAIN_ORIGINAL = bytes([0x8B, 0x44, 0x24, 0x0C, 0x50])  # mov eax,[esp+0xC]; push eax

# --- String data layout ---
STR_KERNEL32  = b"kernel32.dll\x00"                  # at CAVE_STRINGS_VA + 0
STR_SETCURDIR = b"SetCurrentDirectoryA\x00"          # at CAVE_STRINGS_VA + 13
STR_DATA      = b"data\x00"                          # at CAVE_STRINGS_VA + 34

STR_KERNEL32_VA  = CAVE_STRINGS_VA + 0
STR_SETCURDIR_VA = CAVE_STRINGS_VA + 13
STR_DATA_VA      = CAVE_STRINGS_VA + 34

assert len(STR_KERNEL32) + len(STR_SETCURDIR) + len(STR_DATA) == 39

# --- Build trampoline code ---
def build_trampoline():
    code = bytearray()

    # pushad - preserve all registers
    code += b'\x60'                                                   # +0  (1)

    # GetModuleHandleA("kernel32.dll")
    code += b'\x68' + struct.pack('<I', STR_KERNEL32_VA)              # +1  push "kernel32.dll"  (5)
    code += b'\xFF\x15' + struct.pack('<I', IAT_GETMODHDL)            # +6  call [GetModuleHandleA]  (6)

    # GetProcAddress(hKernel32, "SetCurrentDirectoryA")
    code += b'\x68' + struct.pack('<I', STR_SETCURDIR_VA)             # +12 push "SetCurrentDirectoryA"  (5)
    code += b'\x50'                                                   # +17 push eax (hModule)  (1)
    code += b'\xFF\x15' + struct.pack('<I', IAT_GETPROCADDR)          # +18 call [GetProcAddress]  (6)

    # SetCurrentDirectoryA("data")
    code += b'\x68' + struct.pack('<I', STR_DATA_VA)                  # +24 push "data"  (5)
    code += b'\xFF\xD0'                                               # +29 call eax  (2)

    # popad - restore all registers
    code += b'\x61'                                                   # +31 (1)

    # Execute displaced instructions from WinMain entry
    code += b'\x8B\x44\x24\x0C'                                      # +32 mov eax,[esp+0xC]  (4)
    code += b'\x50'                                                   # +36 push eax  (1)

    # Jump back to WinMain+5
    jmp_back_target = WINMAIN_VA + 5
    jmp_back_source = CAVE_CODE_VA + len(code) + 5  # +5 for the jmp instruction itself
    jmp_back_offset = jmp_back_target - jmp_back_source
    code += b'\xE9' + struct.pack('<i', jmp_back_offset)              # +37 jmp WinMain+5  (5)

    assert len(code) == 42, f"Trampoline is {len(code)} bytes, expected 42"
    return bytes(code)

# --- Build WinMain redirect ---
def build_winmain_jmp():
    jmp_target = CAVE_CODE_VA
    jmp_source = WINMAIN_VA + 5
    jmp_offset = jmp_target - jmp_source
    return b'\xE9' + struct.pack('<i', jmp_offset)


def find_exe(args):
    """Find TD5_d3d.exe from args or default location."""
    for arg in args:
        if not arg.startswith('--') and os.path.isfile(arg):
            return arg
    # Default: look relative to this script (re/tools/ -> root)
    script_dir = os.path.dirname(os.path.abspath(__file__))
    default = os.path.join(script_dir, '..', '..', 'TD5_d3d.exe')
    if os.path.isfile(default):
        return os.path.abspath(default)
    return None


def main():
    args = sys.argv[1:]
    dry_run = '--dry-run' in args
    revert = '--revert' in args

    exe_path = find_exe(args)
    if not exe_path:
        print("ERROR: TD5_d3d.exe not found. Pass path as argument.")
        sys.exit(1)

    print(f"Target: {exe_path}")

    with open(exe_path, 'rb') as f:
        data = bytearray(f.read())

    # Verify WinMain entry bytes
    current_winmain = bytes(data[WINMAIN_OFF:WINMAIN_OFF + 5])
    expected_jmp = build_winmain_jmp()

    if revert:
        if current_winmain == WINMAIN_ORIGINAL:
            print("Already unpatched — nothing to revert.")
            return
        if current_winmain != expected_jmp:
            print(f"ERROR: Unexpected bytes at WinMain: {current_winmain.hex()}")
            print("Cannot safely revert — unknown state.")
            sys.exit(1)

        print("Reverting CWD patch...")
        # Restore WinMain entry
        data[WINMAIN_OFF:WINMAIN_OFF + 5] = WINMAIN_ORIGINAL
        # Zero out cave data
        data[CAVE_STRINGS_OFF:CAVE_STRINGS_OFF + 39] = b'\x00' * 39
        data[CAVE_CODE_OFF:CAVE_CODE_OFF + 42] = b'\x00' * 42

        if dry_run:
            print("[DRY RUN] Would revert 86 bytes. No changes written.")
        else:
            with open(exe_path, 'wb') as f:
                f.write(data)
            print("Reverted successfully.")
        return

    if current_winmain == expected_jmp:
        print("Already patched — CWD trampoline is in place.")
        return

    if current_winmain != WINMAIN_ORIGINAL:
        print(f"ERROR: Unexpected bytes at WinMain (0x{WINMAIN_VA:X}):")
        print(f"  Expected: {WINMAIN_ORIGINAL.hex()}")
        print(f"  Found:    {current_winmain.hex()}")
        print("Binary may already be modified by another patch.")
        sys.exit(1)

    # Verify cave area is empty
    cave_region = bytes(data[CAVE_STRINGS_OFF:CAVE_STRINGS_OFF + 39])
    cave_region += bytes(data[CAVE_CODE_OFF:CAVE_CODE_OFF + 42])
    if any(b != 0 for b in cave_region):
        print("ERROR: Cave area (0x45C110-0x45C16A) is not empty.")
        print("Another patch may occupy this region.")
        sys.exit(1)

    # Build patch data
    string_data = STR_KERNEL32 + STR_SETCURDIR + STR_DATA
    trampoline = build_trampoline()
    winmain_jmp = build_winmain_jmp()

    print(f"\nPatch details:")
    print(f"  Strings at  0x{CAVE_STRINGS_VA:08X} ({len(string_data)} bytes)")
    print(f"  Code at     0x{CAVE_CODE_VA:08X} ({len(trampoline)} bytes)")
    print(f"  JMP at      0x{WINMAIN_VA:08X} ({len(winmain_jmp)} bytes)")
    print(f"  IAT GetProcAddress:   0x{IAT_GETPROCADDR:08X}")
    print(f"  IAT GetModuleHandleA: 0x{IAT_GETMODHDL:08X}")
    print(f"  Total: {len(string_data) + len(trampoline) + len(winmain_jmp)} bytes")
    print()
    print(f"  Trampoline hex: {trampoline.hex()}")
    print(f"  WinMain JMP:    {winmain_jmp.hex()}")

    if dry_run:
        print("\n[DRY RUN] No changes written.")
        return

    # Create backup
    backup = exe_path + '.bak_cwd'
    if not os.path.exists(backup):
        shutil.copy2(exe_path, backup)
        print(f"\nBackup: {backup}")

    # Apply patches
    data[CAVE_STRINGS_OFF:CAVE_STRINGS_OFF + len(string_data)] = string_data
    data[CAVE_CODE_OFF:CAVE_CODE_OFF + len(trampoline)] = trampoline
    data[WINMAIN_OFF:WINMAIN_OFF + len(winmain_jmp)] = winmain_jmp

    with open(exe_path, 'wb') as f:
        f.write(data)

    print("\nPatch applied successfully.")
    print("Next: move game data files into data/ subfolder.")
    print("IMPORTANT: TD5.ini must also be copied to data/ (game reads it from CWD).")


if __name__ == '__main__':
    main()
