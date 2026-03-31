"""
patch_car_bg_centering.py

Fixes car background centering at 2560x1440 in FUN_0040d830.

FUN_0040d830 draws the car image DIRECTLY to the back-buffer via
FUN_0040d190 (software pixel-blending path, active when DAT_00495234 != 0).
It uses DAT_0048f2f4=0x76 (118) and DAT_0048f2f8=0x8c (140) as the
destination (x, y) on the back-buffer. These are hardcoded 640x480
coordinates, so the car ends up at the top-left of the 2560x1440 surface.

There are two CALL-to-FUN_0040d190 sites inside FUN_0040d830:
  Site 1: VA 0x40d8fa  — non-animation path (DAT_004951dc == 0)
  Site 2: VA 0x40da53  — animation path    (DAT_004951dc == 2)

At both CALL sites the stack layout is identical (pushed right-to-left):
  [ESP+00] = uVar1  (alpha-in,  param_1)
  [ESP+04] = uVar3  (alpha-out, param_2)
  [ESP+08] = x      (DAT_0048f2f4, param_3)   ← ADD 960 here
  [ESP+0C] = y      (DAT_0048f2f8, param_4)   ← ADD 480 here
  [ESP+10] = surf   (DAT_0048f304, param_5)

Fix: replace each 5-byte CALL with a JMP to a code cave that (when
game_state==1) adds +960/+480 to [ESP+8]/[ESP+C] before calling
FUN_0040d190, then returns to the original next instruction.

The BltFast path (site 3 at 0x40da83, active when DAT_00495234 == 0)
calls FUN_004251a0 which already goes through the BltFast trampoline
at 0x45C1A1 (state==1, adds +960/+480). However dgVoodoo2 ignores
BltFast destX/destY for flag=0x10. That path is NOT the confirmed
active path (DAT_00495234 appears non-zero at runtime), so it's left
for a follow-up fix if ever needed.

Code cave layout (free area confirmed all-zero):
  CAVE_SITE1_VA = 0x45C2B0  (35 bytes, site 1 cave)
  CAVE_SITE2_VA = 0x45C2D3  (35 bytes, site 2 cave)
"""

import struct, os

EXE        = "D:/Descargas/Test Drive 5 ISO/TD5_d3d.exe"
IMAGE_BASE = 0x400000

TARGET_W, TARGET_H = 2560, 1440
ORIG_W,   ORIG_H   =  640,  480
OFF_X = (TARGET_W - ORIG_W) // 2   # 960 = 0x3C0
OFF_Y = (TARGET_H - ORIG_H) // 2   # 480 = 0x1E0

GAME_STATE_VA  = 0x4C3CE8
FUNC_D190_VA   = 0x0040D190   # FUN_0040d190 (software pixel-blend)

SITE1_INTERCEPT_VA = 0x0040D8FA   # CALL 0x40d190  E8 91 f8 ff ff
SITE1_RETURN_VA    = 0x0040D8FF   # next: ADD ESP, 0x14

SITE2_INTERCEPT_VA = 0x0040DA53   # CALL 0x40d190  E8 38 f7 ff ff
SITE2_RETURN_VA    = 0x0040DA58   # next: MOV EDI, [0x48f304]

CAVE_SITE1_VA  = 0x45C2B0   # free space after existing Blt code cave
CAVE_SITE2_VA  = 0x45C2D3   # immediately after site 1 cave (35 bytes later)

fo      = lambda va: va - IMAGE_BASE
u32     = lambda x: struct.pack('<I', x)
rel32   = lambda frm, to: struct.pack('<i', to - (frm + 5))


def build_cave(cave_va, return_va):
    """
    Code cave (35 bytes):
      CMP [game_state], 1   ; 7 bytes
      JNE .skip             ; 2 bytes  (+16 skips the two ADDs)
      ADD [ESP+8],  OFF_X   ; 8 bytes  (x += 960)
      ADD [ESP+C],  OFF_Y   ; 8 bytes  (y += 480)
    .skip:
      CALL FUN_0040d190     ; 5 bytes
      JMP  return_va        ; 5 bytes
    """
    c  = bytearray()
    va = cave_va

    # CMP DWORD [GAME_STATE_VA], 1
    c += b'\x83\x3D' + u32(GAME_STATE_VA) + b'\x01'; va += 7

    # JNE .skip  (rel8, skips 8+8=16 bytes of ADDs)
    jne_off = len(c)
    c += b'\x75\x10'; va += 2

    # ADD DWORD [ESP+0x08], OFF_X
    c += b'\x81\x44\x24\x08' + u32(OFF_X); va += 8
    # ADD DWORD [ESP+0x0C], OFF_Y
    c += b'\x81\x44\x24\x0C' + u32(OFF_Y); va += 8

    # .skip: verify JNE offset
    assert len(c) - (jne_off + 2) == 16, "JNE rel8 mismatch"

    # CALL FUN_0040d190
    c += b'\xE8' + rel32(va, FUNC_D190_VA); va += 5
    # JMP return_va
    c += b'\xE9' + rel32(va, return_va);    va += 5

    assert len(c) == 35, f"cave size mismatch: {len(c)}"
    assert va == cave_va + len(c)
    return bytes(c)


def apply():
    cave1 = build_cave(CAVE_SITE1_VA, SITE1_RETURN_VA)
    cave2 = build_cave(CAVE_SITE2_VA, SITE2_RETURN_VA)

    jmp1 = b'\xE9' + rel32(SITE1_INTERCEPT_VA, CAVE_SITE1_VA)
    jmp2 = b'\xE9' + rel32(SITE2_INTERCEPT_VA, CAVE_SITE2_VA)

    print(f"OFF_X={OFF_X} OFF_Y={OFF_Y}")
    print(f"Cave 1 ({len(cave1)}B) @ VA 0x{CAVE_SITE1_VA:08X}: {cave1.hex(' ')}")
    print(f"Cave 2 ({len(cave2)}B) @ VA 0x{CAVE_SITE2_VA:08X}: {cave2.hex(' ')}")
    print(f"JMP 1 @ 0x{SITE1_INTERCEPT_VA:08X}: {jmp1.hex(' ')}")
    print(f"JMP 2 @ 0x{SITE2_INTERCEPT_VA:08X}: {jmp2.hex(' ')}")

    with open(EXE, 'rb') as f:
        exe = bytearray(f.read())

    # --- verify intercept sites ---
    expected_call_d190 = b'\xE8' + rel32(SITE1_INTERCEPT_VA, FUNC_D190_VA)
    site1 = bytes(exe[fo(SITE1_INTERCEPT_VA):fo(SITE1_INTERCEPT_VA)+5])
    expected_call2 = b'\xE8' + rel32(SITE2_INTERCEPT_VA, FUNC_D190_VA)
    site2 = bytes(exe[fo(SITE2_INTERCEPT_VA):fo(SITE2_INTERCEPT_VA)+5])

    for label, site, expected, jmp in [
        ("Site1", site1, expected_call_d190, jmp1),
        ("Site2", site2, expected_call2,     jmp2),
    ]:
        if site == jmp:
            print(f"{label}: JMP already present — rewriting cave only.")
        elif site != expected:
            print(f"WARNING {label}: bytes={site.hex(' ')} expected={expected.hex(' ')}")
            if site[0] == 0xE9:
                print(f"  Different JMP already at {label} — aborting.")
                return

    # --- verify cave area is free ---
    cave_span = bytes(exe[fo(CAVE_SITE1_VA):fo(CAVE_SITE2_VA)+len(cave2)])
    if any(b != 0 for b in cave_span):
        print(f"WARNING: cave area not all-zero: {cave_span[:32].hex(' ')}")
        print("Proceeding anyway.")

    # --- apply caves ---
    exe[fo(CAVE_SITE1_VA):fo(CAVE_SITE1_VA)+len(cave1)] = cave1
    exe[fo(CAVE_SITE2_VA):fo(CAVE_SITE2_VA)+len(cave2)] = cave2

    # --- apply intercept JMPs (only if original CALL present) ---
    if site1 == expected_call_d190:
        exe[fo(SITE1_INTERCEPT_VA):fo(SITE1_INTERCEPT_VA)+5] = jmp1
        print(f"Wrote JMP at 0x{SITE1_INTERCEPT_VA:08X} (file 0x{fo(SITE1_INTERCEPT_VA):06X})")
    if site2 == expected_call2:
        exe[fo(SITE2_INTERCEPT_VA):fo(SITE2_INTERCEPT_VA)+5] = jmp2
        print(f"Wrote JMP at 0x{SITE2_INTERCEPT_VA:08X} (file 0x{fo(SITE2_INTERCEPT_VA):06X})")

    with open(EXE, 'wb') as f:
        f.write(exe)
    print("Done.")


def verify():
    cave1 = build_cave(CAVE_SITE1_VA, SITE1_RETURN_VA)
    cave2 = build_cave(CAVE_SITE2_VA, SITE2_RETURN_VA)
    jmp1  = b'\xE9' + rel32(SITE1_INTERCEPT_VA, CAVE_SITE1_VA)
    jmp2  = b'\xE9' + rel32(SITE2_INTERCEPT_VA, CAVE_SITE2_VA)

    with open(EXE, 'rb') as f:
        exe = f.read()

    for label, intercept_va, jmp, cave_va, cave in [
        ("Site1", SITE1_INTERCEPT_VA, jmp1, CAVE_SITE1_VA, cave1),
        ("Site2", SITE2_INTERCEPT_VA, jmp2, CAVE_SITE2_VA, cave2),
    ]:
        actual_site = exe[fo(intercept_va):fo(intercept_va)+5]
        actual_cave = exe[fo(cave_va):fo(cave_va)+len(cave)]
        print(f"--- {label} ---")
        print(f"  Site:     {actual_site.hex(' ')}  {'OK' if actual_site==jmp else 'MISMATCH'}")
        print(f"  Cave exp: {cave.hex(' ')}")
        print(f"  Cave act: {actual_cave.hex(' ')}  {'OK' if actual_cave==cave else 'MISMATCH'}")


if __name__ == "__main__":
    import sys
    if "--verify" in sys.argv:
        verify()
    else:
        apply()
