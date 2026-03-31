"""
patch_restore_rng.py

Restores the stubbed-out random number generator at VA 0x0042C8D0 in TD5_d3d.exe.

Background:
  FUN_0042C8D0 ("StubbedRandomGenerator") is a 3-byte stub that always returns 0:
      33 C0     XOR EAX,EAX
      C3        RET
  It has 22 callers across 4 functions:
    - FUN_004079C0: collision response (12 calls) -- random tumble on high-energy crashes
    - FUN_0043D830: vertex color perturbation A (4 calls) -- (rand & 7) - 4
    - FUN_0043D910: vertex color perturbation B (4 calls) -- (rand & 7) - 4
    - FUN_0043D9F0: vertex color perturbation C (2 calls) -- (rand & 15) - 8

  With the stub returning 0, collision tumble is deterministic (always -param_4/2)
  and vertex shimmer applies a constant -4 or -8 bias instead of random jitter.

Fix:
  Replace the XOR EAX,EAX; RET with a JMP to the existing CRT _rand function
  at VA 0x00448157.  _rand takes no parameters, returns int (0..0x7FFF) in EAX,
  and uses __cdecl calling convention.  The stub uses __stdcall with no parameters,
  so the conventions are identical (no stack cleanup difference with 0 args).

  The JMP is a near relative jump (E9 rel32).  _rand's own RET will return
  directly to the original caller of the stub.

  Patch bytes at VA 0x0042C8D0 (file offset 0x2C8D0):
      E9 82 B8 01 00      JMP _rand  (0x42C8D0+5+0x1B882 = 0x448157)

  The remaining 11 bytes (0x42C8D5..0x42C8DF) are NOP padding before the next
  function at 0x42C8E0 and are left untouched (already 0x90).

Expected effect:
  - Collision tumble: crashes above 90000 energy now add random rotation/position
    perturbation, making post-collision car behavior visually dynamic
  - Vertex shimmer: car model vertex colors gain subtle per-frame random jitter,
    restoring the intended "heat shimmer" / paint sparkle visual effect

Risk: LOW
  - _rand is the MSVC CRT rand(), well-tested and thread-safe (uses TLS seed)
  - Returns 0..0x7FFF which is within the range all 22 callers expect
  - No stack imbalance: both conventions are equivalent with zero parameters
  - Worst case: visual change is too dramatic -- easily reverted
"""

import struct
import sys
import os

# --- Configuration -----------------------------------------------------------

IMAGE_BASE = 0x00400000

# The stubbed RNG function
STUB_VA    = 0x0042C8D0
STUB_FO    = STUB_VA - IMAGE_BASE   # File offset: 0x2C8D0

# CRT _rand function
RAND_VA    = 0x00448157

# Original bytes (XOR EAX,EAX; RET)
ORIGINAL_BYTES = bytes([0x33, 0xC0, 0xC3])

# Patch: JMP rel32 to _rand
# rel32 = target - (source + 5) = 0x448157 - 0x42C8D5 = 0x1B882
JMP_REL32  = RAND_VA - (STUB_VA + 5)
PATCH_BYTES = bytes([0xE9]) + struct.pack('<i', JMP_REL32)

assert len(PATCH_BYTES) == 5
assert JMP_REL32 == 0x1B882, f"Unexpected rel32: 0x{JMP_REL32:X}"

# --- Helpers -----------------------------------------------------------------

def find_exe():
    """Look for TD5_d3d.exe in common locations."""
    candidates = [
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "TD5_d3d.exe"),
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "TD5_d3d.exe"),
        "TD5_d3d.exe",
    ]
    for c in candidates:
        p = os.path.normpath(c)
        if os.path.isfile(p):
            return p
    return None


def apply(exe_path):
    """Apply the RNG restoration patch."""
    with open(exe_path, 'rb') as f:
        exe = bytearray(f.read())

    current = bytes(exe[STUB_FO:STUB_FO + 5])
    print(f"Target:  VA 0x{STUB_VA:08X}  (file offset 0x{STUB_FO:06X})")
    print(f"Current: {current.hex(' ')}")

    # Already patched?
    if current[:5] == PATCH_BYTES:
        print("Already patched (JMP _rand) -- nothing to do.")
        return True

    # Verify original bytes
    if current[:3] != ORIGINAL_BYTES:
        print(f"WARNING: expected {ORIGINAL_BYTES.hex(' ')} at start, "
              f"got {current[:3].hex(' ')} -- aborting.")
        print("The binary may have been modified by another patch.")
        return False

    # Apply
    exe[STUB_FO:STUB_FO + 5] = PATCH_BYTES
    with open(exe_path, 'wb') as f:
        f.write(exe)

    print(f"Patched: XOR EAX,EAX; RET  -->  JMP 0x{RAND_VA:08X}")
    print(f"  Bytes: {PATCH_BYTES.hex(' ')}")
    print("Done. Random collision tumble and vertex shimmer are now restored.")
    return True


def revert(exe_path):
    """Revert the patch back to the original stub."""
    with open(exe_path, 'rb') as f:
        exe = bytearray(f.read())

    current = bytes(exe[STUB_FO:STUB_FO + 5])
    print(f"Target:  VA 0x{STUB_VA:08X}  (file offset 0x{STUB_FO:06X})")
    print(f"Current: {current.hex(' ')}")

    if current[:3] == ORIGINAL_BYTES and current[3:5] == bytes([0x90, 0x90]):
        print("Already original (XOR EAX,EAX; RET; NOP; NOP) -- nothing to do.")
        return True

    if current[:5] != PATCH_BYTES:
        print(f"WARNING: expected patch bytes {PATCH_BYTES.hex(' ')}, "
              f"got {current.hex(' ')} -- aborting.")
        return False

    # Restore original + NOP padding
    exe[STUB_FO:STUB_FO + 5] = ORIGINAL_BYTES + bytes([0x90, 0x90])
    with open(exe_path, 'wb') as f:
        f.write(exe)

    print(f"Reverted: JMP _rand  -->  XOR EAX,EAX; RET; NOP; NOP")
    print("Done. RNG stub restored to original (always returns 0).")
    return True


def verify(exe_path):
    """Check the current state of the patch location."""
    with open(exe_path, 'rb') as f:
        exe = f.read()

    current = bytes(exe[STUB_FO:STUB_FO + 5])
    print(f"VA 0x{STUB_VA:08X}  (file offset 0x{STUB_FO:06X}): {current.hex(' ')}")

    if current[:5] == PATCH_BYTES:
        print("  Status: PATCHED (JMP _rand @ 0x{:08X})".format(RAND_VA))
    elif current[:3] == ORIGINAL_BYTES:
        print("  Status: ORIGINAL (XOR EAX,EAX; RET -- always returns 0)")
    else:
        print("  Status: UNKNOWN -- bytes do not match original or patch")

    # Also verify _rand exists at the expected location
    rand_fo = RAND_VA - IMAGE_BASE
    rand_bytes = bytes(exe[rand_fo:rand_fo + 5])
    print(f"\n_rand @ VA 0x{RAND_VA:08X}  (file offset 0x{rand_fo:06X}): {rand_bytes.hex(' ')}")
    # _rand starts with CALL FUN_00449a37: E8 DB 18 00 00
    if rand_bytes == bytes([0xE8, 0xDB, 0x18, 0x00, 0x00]):
        print("  Status: OK (_rand entry point verified)")
    else:
        print("  Status: WARNING -- _rand entry bytes do not match expected pattern")


# --- Main --------------------------------------------------------------------

def main():
    if "--help" in sys.argv or "-h" in sys.argv:
        print("Usage: python patch_restore_rng.py [--verify | --revert] [path/to/TD5_d3d.exe]")
        print()
        print("  (no flag)   Apply the RNG restoration patch")
        print("  --verify    Check current patch state without modifying")
        print("  --revert    Revert patch to original stub")
        return

    # Find exe path from args or auto-detect
    exe_path = None
    for arg in sys.argv[1:]:
        if not arg.startswith("--"):
            exe_path = arg
            break

    if exe_path is None:
        exe_path = find_exe()
        if exe_path is None:
            print("ERROR: Could not find TD5_d3d.exe. Pass the path as an argument.")
            sys.exit(1)

    exe_path = os.path.normpath(exe_path)
    print(f"Executable: {exe_path}")
    print()

    if not os.path.isfile(exe_path):
        print(f"ERROR: File not found: {exe_path}")
        sys.exit(1)

    if "--verify" in sys.argv:
        verify(exe_path)
    elif "--revert" in sys.argv:
        revert(exe_path)
    else:
        if not apply(exe_path):
            sys.exit(1)


if __name__ == "__main__":
    main()
