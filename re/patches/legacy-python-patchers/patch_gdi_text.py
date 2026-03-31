"""
patch_gdi_text.py
=================
Injects a code cave into TD5_d3d.exe that loads td5_gdi_text.dll at runtime
and calls TD5_InitGdiFonts() to install GDI text rendering hooks.

The cave is inserted after all frontend font surfaces have been loaded in
InitializeFrontendResourcesAndState (0x414790).  It replaces the CALL at
0x414A14 (CALL 0x414640) with a CALL to the cave, which:
  1. Calls the original target (0x414640)
  2. LoadLibraryA("td5_gdi_text.dll")
  3. GetProcAddress(hModule, "TD5_InitGdiFonts")
  4. Calls TD5_InitGdiFonts()
  5. Returns to the original code path

If loading fails, the cave falls through silently and the game uses original
bitmap fonts as a fallback.

Usage
-----
  python patch_gdi_text.py           # apply patch
  python patch_gdi_text.py --verify  # check current state
  python patch_gdi_text.py --revert  # restore original bytes

Requires
--------
  td5_gdi_text.dll must be in the same directory as TD5_d3d.exe at runtime.
"""

import struct
import sys

EXE        = "D:/Descargas/Test Drive 5 ISO/TD5_d3d.exe"
IMAGE_BASE = 0x400000

# ── Helpers ──────────────────────────────────────────────────────────────
fo    = lambda va: va - IMAGE_BASE
u32   = lambda x:  struct.pack('<I', x & 0xFFFFFFFF)
rel32 = lambda fr, to: struct.pack('<i', to - (fr + 5))

# ── Key addresses ────────────────────────────────────────────────────────

# IAT slots (KERNEL32.dll imports)
LOADLIBRARY_IAT   = 0x45D02C   # LoadLibraryA
GETPROCADDRESS_IAT = 0x45D0E0  # GetProcAddress

# Injection point: CALL instruction inside InitializeFrontendResourcesAndState
# This CALL is at VA 0x414A14, after all font surfaces are loaded.
# Original bytes: E8 27 FC FF FF  →  CALL 0x414640
INJECT_VA      = 0x414A14
INJECT_TARGET  = 0x414640     # original call target
INJECT_RETURN  = 0x414A19     # next instruction after the CALL
INJECT_ORIG    = bytes([0xE8, 0x27, 0xFC, 0xFF, 0xFF])  # 5 bytes

# Cave location: after existing widescreen caves in .rdata zero-pad
# Existing caves end at ~0x45C577.  We start at 0x45C580 (aligned).
CAVE_VA        = 0x45C580

# String data placed right after cave code
# Layout:
#   CAVE_VA + 0     : cave code (~41 bytes)
#   CAVE_VA + 44    : "td5_gdi_text.dll\0"  (18 bytes)
#   CAVE_VA + 62    : "TD5_InitGdiFonts\0"  (17 bytes)
# Total: ~79 bytes

STR_DLL_VA     = CAVE_VA + 44
STR_INIT_VA    = CAVE_VA + 44 + 17  # right after STR_DLL (17 bytes including null)

# String constants
STR_DLL  = b"td5_gdi_text.dll\x00"   # 17 bytes
STR_INIT = b"TD5_InitGdiFonts\x00"   # 17 bytes


def build_cave():
    """
    Build the code cave that loads the DLL and calls the init function.

    Layout:
      CALL original_target       ; call the function we replaced (0x414640)
      PUSHAD                     ; save all registers
      PUSH str_dll_name          ; "td5_gdi_text.dll"
      CALL [LoadLibraryA]        ; hModule = LoadLibraryA(...)
      TEST EAX, EAX
      JZ .done                   ; if NULL, skip (DLL not found)
      PUSH str_init_name         ; "TD5_InitGdiFonts"
      PUSH EAX                   ; hModule
      CALL [GetProcAddress]      ; pfnInit = GetProcAddress(...)
      TEST EAX, EAX
      JZ .done                   ; if NULL, skip (export not found)
      CALL EAX                   ; pfnInit()  →  TD5_InitGdiFonts()
    .done:
      POPAD                      ; restore all registers
      RET                        ; return to InitializeFrontendResourcesAndState
    """
    c  = bytearray()
    va = CAVE_VA

    # CALL original_target  (E8 rel32)
    c += b'\xE8' + rel32(va, INJECT_TARGET)
    va += 5

    # PUSHAD
    c += b'\x60'
    va += 1

    # PUSH str_dll_name  (68 imm32)
    c += b'\x68' + u32(STR_DLL_VA)
    va += 5

    # CALL [LoadLibraryA]  (FF 15 imm32)
    c += b'\xFF\x15' + u32(LOADLIBRARY_IAT)
    va += 6

    # TEST EAX, EAX  (85 C0)
    c += b'\x85\xC0'
    va += 2

    # JZ .done  (74 XX)
    jz1_off = len(c)
    c += b'\x74\x00'
    va += 2

    # PUSH str_init_name  (68 imm32)
    c += b'\x68' + u32(STR_INIT_VA)
    va += 5

    # PUSH EAX  (hModule)
    c += b'\x50'
    va += 1

    # CALL [GetProcAddress]  (FF 15 imm32)
    c += b'\xFF\x15' + u32(GETPROCADDRESS_IAT)
    va += 6

    # TEST EAX, EAX  (85 C0)
    c += b'\x85\xC0'
    va += 2

    # JZ .done  (74 XX)
    jz2_off = len(c)
    c += b'\x74\x00'
    va += 2

    # CALL EAX  (FF D0)
    c += b'\xFF\xD0'
    va += 2

    # .done:
    done_off = len(c)
    c[jz1_off + 1] = done_off - (jz1_off + 2)
    c[jz2_off + 1] = done_off - (jz2_off + 2)

    # POPAD
    c += b'\x61'
    va += 1

    # RET
    c += b'\xC3'
    va += 1

    code_len = len(c)
    assert code_len <= 44, f"Cave code {code_len}B > 44B limit"

    # Pad to 44 bytes (before string data)
    c += b'\x90' * (44 - code_len)

    # Append string data
    c += STR_DLL
    assert len(c) == 44 + len(STR_DLL)

    # Pad to align STR_INIT
    c += STR_INIT

    total = len(c)
    print(f"  Cave code: {code_len}B  |  Total with strings: {total}B")
    return bytes(c)


def build_intercept():
    """5-byte CALL rel32 to redirect INJECT_VA to CAVE_VA."""
    return b'\xE8' + rel32(INJECT_VA, CAVE_VA)


def apply():
    cave      = build_cave()
    intercept = build_intercept()

    with open(EXE, 'rb') as f:
        exe = bytearray(f.read())

    # --- Write cave code + strings ---
    print(f"\n[cave @ 0x{CAVE_VA:08X}]")
    exe[fo(CAVE_VA):fo(CAVE_VA) + len(cave)] = cave
    print(f"  Wrote {len(cave)}B at file offset 0x{fo(CAVE_VA):05X}")

    # --- Write intercept ---
    print(f"\n[intercept @ 0x{INJECT_VA:08X}]")
    site = bytes(exe[fo(INJECT_VA):fo(INJECT_VA) + 5])
    if site == intercept:
        print(f"  Already patched")
    elif site == INJECT_ORIG:
        exe[fo(INJECT_VA):fo(INJECT_VA) + 5] = intercept
        print(f"  {site.hex(' ')} -> {intercept.hex(' ')}")
    else:
        print(f"  WARNING: unexpected bytes {site.hex(' ')}")
        print(f"  Expected original: {INJECT_ORIG.hex(' ')}")
        print(f"  Overwriting anyway.")
        exe[fo(INJECT_VA):fo(INJECT_VA) + 5] = intercept

    with open(EXE, 'wb') as f:
        f.write(exe)

    print(f"\nDone.  Ensure td5_gdi_text.dll is next to TD5_d3d.exe.")


def verify():
    cave      = build_cave()
    intercept = build_intercept()

    with open(EXE, 'rb') as f:
        exe = f.read()

    all_ok = True

    def chk(label, va, expected):
        nonlocal all_ok
        actual = exe[fo(va):fo(va) + len(expected)]
        ok = actual == expected
        if not ok:
            all_ok = False
        status = "OK" if ok else "MISMATCH"
        print(f"  {label:50s} [{status}]")
        if not ok:
            print(f"    expected: {expected.hex(' ')}")
            print(f"    actual:   {bytes(actual).hex(' ')}")

    print("[cave]")
    chk(f"Cave code+data ({len(cave)}B) @ 0x{CAVE_VA:08X}", CAVE_VA, cave)

    print("\n[intercept]")
    chk(f"CALL redirect @ 0x{INJECT_VA:08X}", INJECT_VA, intercept)

    print()
    print("All OK" if all_ok else "SOME PATCHES MISSING OR MISMATCHED")


def revert():
    with open(EXE, 'rb') as f:
        exe = bytearray(f.read())

    site = bytes(exe[fo(INJECT_VA):fo(INJECT_VA) + 5])

    if site == INJECT_ORIG:
        print("Already at original bytes -- nothing to revert.")
        return

    intercept = build_intercept()
    if site != intercept:
        print(f"WARNING: unexpected bytes at 0x{INJECT_VA:08X}: {site.hex(' ')}")
        print(f"Expected patched: {intercept.hex(' ')}")
        print("Restoring original bytes anyway.")

    exe[fo(INJECT_VA):fo(INJECT_VA) + 5] = INJECT_ORIG
    print(f"Restored original CALL at 0x{INJECT_VA:08X}: {INJECT_ORIG.hex(' ')}")

    # Zero out cave (optional cleanup)
    cave = build_cave()
    exe[fo(CAVE_VA):fo(CAVE_VA) + len(cave)] = b'\x00' * len(cave)
    print(f"Zeroed cave at 0x{CAVE_VA:08X}")

    with open(EXE, 'wb') as f:
        f.write(exe)
    print("Reverted.")


if __name__ == "__main__":
    if "--verify" in sys.argv:
        verify()
    elif "--revert" in sys.argv:
        revert()
    else:
        apply()
