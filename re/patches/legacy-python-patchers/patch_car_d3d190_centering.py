"""
patch_car_d3d190_centering.py

Fixes car image centering at 2560x1440 by patching INSIDE FUN_0040d190.

Problem with previous approach (patch_car_bg_centering.py):
  FUN_0040d190(alpha_in, alpha_out, x, y, car_surf) uses (x, y) for:
    1. Source read from DAT_00496260 (composite surface, 640x480)
    2. Destination write to DAT_00495220 (back buffer, 2560x1440)
  Adding +960/+480 to x,y before the call puts the composite read at
  (1078, 620) which is OUT OF BOUNDS for the 640x480 surface -> reads
  zeros -> corrupted/black blend output.

Correct fix:
  Intercept at VA 0x40d369 (inside FUN_0040d190, after the two earlier
  locks have returned), at the exact point where the back buffer write
  pointer is computed. Add the centering byte offset ONLY to the back
  buffer pointer, not to the composite read pointer.

  Back buffer centering byte offset:
    (960 + 480 * 2560) * 2 = (960 + 1228800) * 2 = 2459520 = 0x258780
  This shifts the write pointer from (x, y) to (x+960, y+480).

  Composite read pointer (computed earlier at 0x40d278) is NOT modified;
  it uses the original (x, y) and stays in-bounds for the 640x480 surface.

Intercept:
  VA 0x40d369, 6 bytes: 8D 04 42 89 45 14
    LEA EAX, [EDX + EAX*2]    ; compute back buf write ptr at (x, y)
    MOV [EBP + 0x14], EAX     ; store it

  Replace with: E9 xx xx xx xx 90  (JMP to cave + NOP)

Cave at 0x45C2F6 (25 bytes):
  LEA EAX, [EDX + EAX*2]         ; original computation
  CMP DWORD [game_state], 1       ; menu?
  JNE .skip                       ; no -> leave ptr as-is
  ADD EAX, 0x258780               ; yes -> shift to centered position
.skip:
  MOV [EBP + 0x14], EAX           ; store (original instruction 2)
  JMP 0x40d36f                    ; return to after the 6 replaced bytes

Reverts:
  Also restores the original CALLs at 0x40d8fa and 0x40da53 that
  patch_car_bg_centering.py replaced with JMPs. Those caves (0x45C2B0,
  0x45C2D3) become dead code and are left in place.
"""

import struct

EXE        = "D:/Descargas/Test Drive 5 ISO/TD5_d3d.exe"
IMAGE_BASE = 0x400000

GAME_STATE_VA = 0x4C3CE8
OFF_BYTES     = 2459520  # (960 + 480*2560) * 2  bytes to add to write ptr

INTERCEPT_VA  = 0x0040D369   # LEA EAX,[EDX+EAX*2]; MOV [EBP+0x14],EAX — 6 bytes
RETURN_VA     = 0x0040D36F   # CMP [0x49525c], 0xf  (next instruction)
CAVE_VA       = 0x0045C2F6   # free after car bg caves (0x45C2D3 + 35 = 0x45C2F6)

# Original call sites that patch_car_bg_centering.py replaced with JMPs
FUNC_D190_VA       = 0x0040D190
SITE1_INTERCEPT_VA = 0x0040D8FA
SITE1_RETURN_VA    = 0x0040D8FF
SITE2_INTERCEPT_VA = 0x0040DA53
SITE2_RETURN_VA    = 0x0040DA58

fo    = lambda va: va - IMAGE_BASE
u32   = lambda x: struct.pack('<I', x)
rel32 = lambda frm, to: struct.pack('<i', to - (frm + 5))


ORIG_BYTES = bytes([0x8D, 0x04, 0x42,   # LEA EAX, [EDX + EAX*2]
                    0x89, 0x45, 0x14])  # MOV [EBP+0x14], EAX


def build_cave():
    """
    25 bytes:
      8D 04 42              LEA EAX, [EDX + EAX*2]
      83 3D xx xx xx xx 01  CMP DWORD [GAME_STATE_VA], 1
      75 05                 JNE +5  (.skip)
      05 xx xx xx xx        ADD EAX, OFF_BYTES
    .skip:
      89 45 14              MOV [EBP + 0x14], EAX
      E9 xx xx xx xx        JMP RETURN_VA
    """
    c  = bytearray()
    va = CAVE_VA

    # LEA EAX, [EDX + EAX*2]
    c += b'\x8D\x04\x42'; va += 3

    # CMP DWORD [GAME_STATE_VA], 1
    c += b'\x83\x3D' + u32(GAME_STATE_VA) + b'\x01'; va += 7

    # JNE .skip (+5 = over the ADD)
    c += b'\x75\x05'; va += 2

    # ADD EAX, OFF_BYTES
    c += b'\x05' + u32(OFF_BYTES); va += 5

    # .skip: MOV [EBP + 0x14], EAX
    c += b'\x89\x45\x14'; va += 3

    # JMP RETURN_VA
    c += b'\xE9' + rel32(va, RETURN_VA); va += 5

    assert len(c) == 25, f"cave size mismatch: {len(c)}"
    assert va == CAVE_VA + 25
    return bytes(c)


def build_intercept_jmp():
    """6 bytes: JMP to cave + NOP"""
    jmp = b'\xE9' + rel32(INTERCEPT_VA, CAVE_VA) + b'\x90'
    assert len(jmp) == 6
    return jmp


def build_original_calls():
    call1 = b'\xE8' + rel32(SITE1_INTERCEPT_VA, FUNC_D190_VA)
    call2 = b'\xE8' + rel32(SITE2_INTERCEPT_VA, FUNC_D190_VA)
    return call1, call2


def apply():
    cave    = build_cave()
    jmp     = build_intercept_jmp()
    call1, call2 = build_original_calls()

    print(f"Cave  ({len(cave)}B) @ VA 0x{CAVE_VA:08X}: {cave.hex(' ')}")
    print(f"JMP   ({len(jmp)}B)  @ VA 0x{INTERCEPT_VA:08X}: {jmp.hex(' ')}")
    print(f"CALL1 (5B)  @ VA 0x{SITE1_INTERCEPT_VA:08X}: {call1.hex(' ')}")
    print(f"CALL2 (5B)  @ VA 0x{SITE2_INTERCEPT_VA:08X}: {call2.hex(' ')}")

    with open(EXE, 'rb') as f:
        exe = bytearray(f.read())

    # --- verify / check cave area free ---
    cave_span = exe[fo(CAVE_VA):fo(CAVE_VA) + len(cave)]
    if any(b != 0 for b in cave_span):
        print(f"WARNING: cave area not all-zero: {bytes(cave_span).hex(' ')}")
        print("Proceeding anyway.")

    # --- verify intercept site ---
    site = bytes(exe[fo(INTERCEPT_VA):fo(INTERCEPT_VA) + 6])
    if site == jmp:
        print("JMP already present at intercept site — rewriting cave only.")
        refix = True
    elif site != ORIG_BYTES:
        print(f"WARNING: intercept site = {site.hex(' ')} expected {ORIG_BYTES.hex(' ')}")
        refix = False
    else:
        refix = False

    # --- check / revert call-site patches ---
    for label, va, expected_call in [
        ("Site1", SITE1_INTERCEPT_VA, call1),
        ("Site2", SITE2_INTERCEPT_VA, call2),
    ]:
        actual = bytes(exe[fo(va):fo(va)+5])
        if actual == expected_call:
            print(f"{label}: already CALL (original or already reverted) — no change needed.")
        elif actual[0] == 0xE9:
            print(f"{label}: JMP present — restoring original CALL {expected_call.hex(' ')}")
            exe[fo(va):fo(va)+5] = expected_call
        else:
            print(f"WARNING {label}: unexpected bytes {actual.hex(' ')} — skipping revert.")

    # --- apply cave ---
    exe[fo(CAVE_VA):fo(CAVE_VA) + len(cave)] = cave

    # --- apply intercept JMP ---
    if not refix:
        exe[fo(INTERCEPT_VA):fo(INTERCEPT_VA) + 6] = jmp
        print(f"Wrote JMP at VA 0x{INTERCEPT_VA:08X} (file 0x{fo(INTERCEPT_VA):06X})")

    with open(EXE, 'wb') as f:
        f.write(exe)
    print("Done.")


def verify():
    cave    = build_cave()
    jmp     = build_intercept_jmp()
    call1, call2 = build_original_calls()

    with open(EXE, 'rb') as f:
        exe = f.read()

    site = exe[fo(INTERCEPT_VA):fo(INTERCEPT_VA)+6]
    s_ok = "OK" if site == jmp else ("ORIG" if site == ORIG_BYTES else "MISMATCH")
    print(f"Intercept VA 0x{INTERCEPT_VA:08X}: {bytes(site).hex(' ')}  [{s_ok}]")

    actual_cave = exe[fo(CAVE_VA):fo(CAVE_VA)+len(cave)]
    c_ok = "OK" if actual_cave == cave else "MISMATCH"
    print(f"Cave     VA 0x{CAVE_VA:08X}: {bytes(actual_cave).hex(' ')}  [{c_ok}]")

    for label, va, expected_call in [
        ("Site1", SITE1_INTERCEPT_VA, call1),
        ("Site2", SITE2_INTERCEPT_VA, call2),
    ]:
        actual = exe[fo(va):fo(va)+5]
        ok = "CALL (reverted)" if actual == expected_call else ("JMP (old patch)" if actual[0] == 0xE9 else "OTHER")
        print(f"{label}  VA 0x{va:08X}: {bytes(actual).hex(' ')}  [{ok}]")


if __name__ == "__main__":
    import sys
    if "--verify" in sys.argv:
        verify()
    else:
        apply()
