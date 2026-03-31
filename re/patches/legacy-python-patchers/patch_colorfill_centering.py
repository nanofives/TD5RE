"""
Intercepts FUN_00424050 (Blt COLORFILL) INSIDE the function, after the
RECT is constructed, to add centering offset when in frontend state.

Intercept site: VA 0x4240e7 (7 bytes: MOV EAX,[ESP+0x8C])
  - At this point [ESP+0]=left, [ESP+4]=top, [ESP+8]=right, [ESP+C]=bottom
  - Replaces the 7-byte MOV with JMP + 2 NOPs -> trampoline

Trampoline at code cave 0x45C1F5 (after existing BltFast trampoline):
  - If DAT_004c3ce8 == 3 (frontend): ADD add_x/add_y to all 4 rect fields
  - Reproduces overwritten MOV EAX,[ESP+0x8C]
  - JMPs back to 0x4240ee

Safe because:
  - SUB ESP,0x74 already executed (stack frame stable)
  - We modify [ESP+0..F] which are locals this function owns
  - Race calls have game_state != 3, so offset is not applied
"""

import struct, shutil, os

EXE     = "D:/Descargas/Test Drive 5 ISO/TD5_d3d.exe"
BAK     = EXE + ".bak3"   # use existing safe backup

# -- constants
TARGET_W, TARGET_H = 2560, 1440
ORIG_W,   ORIG_H   = 640,  480
ADD_X = (TARGET_W - ORIG_W) // 2   # 960  = 0x3C0
ADD_Y = (TARGET_H - ORIG_H) // 2   # 480  = 0x1E0

GAME_STATE_VA    = 0x4C3CE8
INTERCEPT_VA     = 0x4240e7   # 7-byte MOV EAX,[ESP+0x8C]
RETURN_VA        = 0x4240ee   # next instruction after intercept
TRAMPOLINE_VA    = 0x45C1F5   # free space after BltFast trampoline

INTERCEPT_FILE   = INTERCEPT_VA  - 0x400000   # 0x0240e7
TRAMPOLINE_FILE  = TRAMPOLINE_VA - 0x400000   # 0x05C1F5

def i32(x):
    return struct.pack('<i', x)

def u32(x):
    return struct.pack('<I', x)

def rel32(from_va, to_va):
    return struct.pack('<i', to_va - (from_va + 5))

def build_trampoline():
    GS  = u32(GAME_STATE_VA)
    ax  = u32(ADD_X)
    ay  = u32(ADD_Y)

    code = bytearray()

    # CMP DWORD [GAME_STATE], 3
    code += bytes([0x83, 0x3D]) + GS + bytes([0x03])   # 7 bytes

    # JNE .skip  (8-bit rel, filled below)
    jne_off = len(code)
    code += bytes([0x75, 0x00])                        # 2 bytes placeholder

    # ADD DWORD [ESP+0x00], add_x  (RECT.left)
    code += bytes([0x81, 0x04, 0x24]) + ax             # 7 bytes
    # ADD DWORD [ESP+0x04], add_y  (RECT.top)
    code += bytes([0x81, 0x44, 0x24, 0x04]) + ay       # 7 bytes
    # ADD DWORD [ESP+0x08], add_x  (RECT.right)
    code += bytes([0x81, 0x44, 0x24, 0x08]) + ax       # 7 bytes
    # ADD DWORD [ESP+0x0C], add_y  (RECT.bottom)
    code += bytes([0x81, 0x44, 0x24, 0x0C]) + ay       # 7 bytes

    # .skip:
    skip_off = len(code)
    # patch JNE rel8
    code[jne_off + 1] = skip_off - (jne_off + 2)

    # Reproduce overwritten instruction: MOV EAX,[ESP+0x8C]
    code += bytes([0x8B, 0x84, 0x24, 0x8C, 0x00, 0x00, 0x00])   # 7 bytes

    # JMP back to RETURN_VA
    jmp_va = TRAMPOLINE_VA + len(code)
    code += b'\xE9' + rel32(jmp_va, RETURN_VA)         # 5 bytes

    return bytes(code)


def apply():
    trampoline = build_trampoline()
    print(f"Trampoline: {len(trampoline)} bytes at file 0x{TRAMPOLINE_FILE:06X} (VA 0x{TRAMPOLINE_VA:08X})")
    print(f"  add_x={ADD_X} add_y={ADD_Y}")
    print(f"  bytes: {trampoline.hex(' ')}")

    # Verify backup exists
    if not os.path.exists(BAK):
        print(f"ERROR: backup not found at {BAK}")
        return

    with open(EXE, 'rb') as f:
        data = bytearray(f.read())

    # Verify intercept site looks unpatched
    site = bytes(data[INTERCEPT_FILE:INTERCEPT_FILE+7])
    expected = bytes([0x8B, 0x84, 0x24, 0x8C, 0x00, 0x00, 0x00])
    if site != expected:
        print(f"WARNING: intercept site bytes unexpected: {site.hex(' ')}")
        print(f"         expected:                        {expected.hex(' ')}")
        # If it's already our JMP, skip
        if site[0] == 0xE9:
            print("Already patched (JMP found). Aborting.")
            return

    # Verify trampoline area is zero (free)
    cave = data[TRAMPOLINE_FILE:TRAMPOLINE_FILE+len(trampoline)]
    if any(b != 0 for b in cave):
        print(f"WARNING: code cave not zero at 0x{TRAMPOLINE_FILE:06X}: {cave[:16].hex(' ')}")
        # Proceed anyway (may overlap with prior trampoline placement - check manually)

    # Write trampoline
    data[TRAMPOLINE_FILE:TRAMPOLINE_FILE+len(trampoline)] = trampoline
    print(f"  Trampoline written.")

    # Write JMP redirect (7 bytes -> JMP[5] + NOP[2])
    jmp = b'\xE9' + rel32(INTERCEPT_VA, TRAMPOLINE_VA) + b'\x90\x90'
    assert len(jmp) == 7
    data[INTERCEPT_FILE:INTERCEPT_FILE+7] = jmp
    print(f"  JMP redirect written at file 0x{INTERCEPT_FILE:06X}: {jmp.hex(' ')}")

    with open(EXE, 'wb') as f:
        f.write(data)
    print("Done.")


def verify():
    trampoline = build_trampoline()
    with open(EXE, 'rb') as f:
        data = f.read()
    site = data[INTERCEPT_FILE:INTERCEPT_FILE+7]
    cave = data[TRAMPOLINE_FILE:TRAMPOLINE_FILE+len(trampoline)]
    print(f"Intercept site: {site.hex(' ')}")
    print(f"Trampoline:     {cave.hex(' ')}")
    print(f"Expected tramp: {trampoline.hex(' ')}")
    print(f"Match: {cave == trampoline}")


if __name__ == "__main__":
    import sys
    if "--verify" in sys.argv:
        verify()
    else:
        apply()
