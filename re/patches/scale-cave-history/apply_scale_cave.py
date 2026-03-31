"""
TD5 Widescreen Scale Cave Patch
================================
Applies the remaining widescreen patches to TD5_d3d.exe:

1. Scale cave code (VA 0x45C330) — Blt stretch + COLORFILL bars + Flip
2. Frontend Flip trampoline (VA 0x414D09) — redirects menu Flip to cave
3. Loading screen Flip trampoline (VA 0x42CBBE) — redirects loading/legal Flip to cave
4. Data: srcRect, destRect, pillarbox bar rects, DDBLTFX struct

Target resolution: 2560x1440 with 4:3 pillarbox (1920x1440 centered)
"""

import shutil
import struct
import os

EXE = r"D:\Descargas\Test Drive 5 ISO\TD5_d3d.exe"
BACKUP = EXE + ".pre_scale_cave.bak"

# --- Constants ---
# Pillarbox geometry for 2560x1440 with 4:3 content
DISP_W, DISP_H = 2560, 1440
CONTENT_W = DISP_H * 4 // 3   # 1920
BAR_L = (DISP_W - CONTENT_W) // 2  # 320
BAR_R = BAR_L + CONTENT_W          # 2240
VIRT_W, VIRT_H = 640, 480

# File offsets (= VA - 0x400000)
OFF_DDBLTFX      = 0x5C1A1   # VA 0x45C1A1 — DDBLTFX struct (100 bytes)
OFF_LEFT_BAR     = 0x5C205   # VA 0x45C205 — left bar rect
OFF_RIGHT_BAR    = 0x5C215   # VA 0x45C215 — right bar rect
OFF_SRC_RECT     = 0x5C310   # VA 0x45C310 — source rect {0,0,640,480}
OFF_DST_RECT     = 0x5C320   # VA 0x45C320 — dest rect {320,0,2240,1440}
OFF_CAVE_CODE    = 0x5C330   # VA 0x45C330 — scale cave code
OFF_FRONTEND_JMP = 0x14D09   # VA 0x414D09 — frontend Flip trampoline
OFF_LOADING_JMP  = 0x2CBBE   # VA 0x42CBBE — loading screen Flip trampoline


def make_rect(left, top, right, bottom):
    """RECT struct: 4 x DWORD (little-endian)"""
    return struct.pack('<4I', left, top, right, bottom)


def make_call_rel32(from_va, to_va):
    """E8 rel32 CALL instruction"""
    offset = to_va - (from_va + 5)
    return b'\xE8' + struct.pack('<i', offset)


def build_cave_code():
    """
    Scale cave at VA 0x45C330:
    1. PUSHAD
    2. Get back surface from dd_exref
    3. Blt stretch: back {0,0,640,480} -> back {320,0,2240,1440}
    4. COLORFILL left bar black
    5. COLORFILL right bar black
    6. POPAD
    7. DXDraw::Flip(1)
    8. RET
    """
    code = bytearray()

    # PUSHAD
    code += b'\x60'

    # MOV ECX, [0x45D564]  (dd_exref pointer)
    code += b'\x8B\x0D' + struct.pack('<I', 0x45D564)

    # MOV EBX, [ECX+8]  (back surface / render target)
    code += b'\x8B\x59\x08'

    # --- Blt stretch: back -> back ---
    # IDirectDrawSurface::Blt(this, destRect, srcSurf, srcRect, flags, ddbltfx)
    # COM stdcall: push right-to-left, callee cleans
    code += b'\x6A\x00'                                    # PUSH 0 (lpDDBltFx = NULL)
    code += b'\x68' + struct.pack('<I', 0x01000000)        # PUSH DDBLT_WAIT
    code += b'\x68' + struct.pack('<I', 0x45C310)          # PUSH srcRect addr
    code += b'\x53'                                        # PUSH EBX (source surface)
    code += b'\x68' + struct.pack('<I', 0x45C320)          # PUSH destRect addr
    code += b'\x53'                                        # PUSH EBX (this = back surface)
    code += b'\x8B\x0B'                                    # MOV ECX, [EBX] (vtable)
    code += b'\xFF\x51\x14'                                # CALL [ECX+0x14] (Blt)

    # --- COLORFILL left bar ---
    code += b'\x68' + struct.pack('<I', 0x45C1A1)          # PUSH ddbltfx addr
    code += b'\x68' + struct.pack('<I', 0x01000400)        # PUSH DDBLT_COLORFILL|DDBLT_WAIT
    code += b'\x6A\x00'                                    # PUSH 0 (srcRect = NULL)
    code += b'\x6A\x00'                                    # PUSH 0 (srcSurface = NULL)
    code += b'\x68' + struct.pack('<I', 0x45C205)          # PUSH left bar rect addr
    code += b'\x53'                                        # PUSH EBX (this = back surface)
    code += b'\x8B\x0B'                                    # MOV ECX, [EBX] (vtable)
    code += b'\xFF\x51\x14'                                # CALL [ECX+0x14] (Blt)

    # --- COLORFILL right bar ---
    code += b'\x68' + struct.pack('<I', 0x45C1A1)          # PUSH ddbltfx addr
    code += b'\x68' + struct.pack('<I', 0x01000400)        # PUSH DDBLT_COLORFILL|DDBLT_WAIT
    code += b'\x6A\x00'                                    # PUSH 0 (srcRect = NULL)
    code += b'\x6A\x00'                                    # PUSH 0 (srcSurface = NULL)
    code += b'\x68' + struct.pack('<I', 0x45C215)          # PUSH right bar rect addr
    code += b'\x53'                                        # PUSH EBX (this = back surface)
    code += b'\x8B\x0B'                                    # MOV ECX, [EBX] (vtable)
    code += b'\xFF\x51\x14'                                # CALL [ECX+0x14] (Blt)

    # --- POPAD ---
    code += b'\x61'

    # --- DXDraw::Flip(1) ---
    code += b'\x6A\x01'                                    # PUSH 1
    code += b'\xFF\x15' + struct.pack('<I', 0x45D504)      # CALL [0x45D504] (DXDraw::Flip IAT)
    code += b'\x83\xC4\x04'                                # ADD ESP, 4 (cdecl cleanup)

    # --- RET ---
    code += b'\xC3'

    return bytes(code)


def verify_original(data, offset, expected, label):
    """Verify original bytes before patching"""
    actual = data[offset:offset+len(expected)]
    if actual != expected:
        print(f"  WARNING: {label} at 0x{offset:X} unexpected bytes!")
        print(f"    Expected: {expected.hex()}")
        print(f"    Actual:   {actual.hex()}")
        return False
    return True


def verify_zeros(data, offset, length, label):
    """Verify area is zeros (unused cave space)"""
    actual = data[offset:offset+length]
    if actual != b'\x00' * length:
        print(f"  WARNING: {label} at 0x{offset:X} not all zeros!")
        return False
    return True


def main():
    print("TD5 Widescreen Scale Cave Patch")
    print("=" * 40)

    # Read EXE
    with open(EXE, 'rb') as f:
        data = bytearray(f.read())
    print(f"Read {len(data)} bytes from {os.path.basename(EXE)}")

    # --- Verify preconditions ---
    print("\nVerifying preconditions...")
    ok = True

    # Frontend Flip site: PUSH EBX + CALL [0x45D504] + ADD ESP,4
    ok &= verify_original(data, OFF_FRONTEND_JMP,
        b'\x53\xFF\x15\x04\xD5\x45\x00\x83\xC4\x04', "Frontend Flip")

    # Loading screen Flip site: PUSH EAX + CALL [0x45D504] + ADD ESP,4
    ok &= verify_original(data, OFF_LOADING_JMP,
        b'\x50\xFF\x15\x04\xD5\x45\x00\x83\xC4\x04', "Loading Flip")

    # Cave areas must be zeros
    ok &= verify_zeros(data, OFF_DDBLTFX, 100, "DDBLTFX area")
    ok &= verify_zeros(data, OFF_LEFT_BAR, 16, "Left bar rect area")
    ok &= verify_zeros(data, OFF_RIGHT_BAR, 16, "Right bar rect area")
    ok &= verify_zeros(data, OFF_SRC_RECT, 16, "srcRect area")
    ok &= verify_zeros(data, OFF_DST_RECT, 16, "destRect area")
    ok &= verify_zeros(data, OFF_CAVE_CODE, 97, "Cave code area")

    if not ok:
        print("\nPrecondition check FAILED — aborting!")
        return

    print("All preconditions OK.")

    # --- Backup ---
    if not os.path.exists(BACKUP):
        shutil.copy2(EXE, BACKUP)
        print(f"\nBackup: {os.path.basename(BACKUP)}")
    else:
        print(f"\nBackup already exists: {os.path.basename(BACKUP)}")

    # --- Apply patches ---
    print("\nApplying patches...")

    # 1. DDBLTFX struct (100 bytes, dwSize=0x64 at offset 0, dwFillColor=0 at offset 0x4C)
    data[OFF_DDBLTFX] = 0x64  # dwSize = 100
    # Rest stays zero (dwFillColor = 0 = black)
    print(f"  DDBLTFX struct at 0x{OFF_DDBLTFX:X} (VA 0x{OFF_DDBLTFX+0x400000:X})")

    # 2. Left pillarbox bar rect: {0, 0, 320, 1440}
    rect = make_rect(0, 0, BAR_L, DISP_H)
    data[OFF_LEFT_BAR:OFF_LEFT_BAR+16] = rect
    print(f"  Left bar rect at 0x{OFF_LEFT_BAR:X}: {{0, 0, {BAR_L}, {DISP_H}}}")

    # 3. Right pillarbox bar rect: {2240, 0, 2560, 1440}
    rect = make_rect(BAR_R, 0, DISP_W, DISP_H)
    data[OFF_RIGHT_BAR:OFF_RIGHT_BAR+16] = rect
    print(f"  Right bar rect at 0x{OFF_RIGHT_BAR:X}: {{{BAR_R}, 0, {DISP_W}, {DISP_H}}}")

    # 4. srcRect: {0, 0, 640, 480}
    rect = make_rect(0, 0, VIRT_W, VIRT_H)
    data[OFF_SRC_RECT:OFF_SRC_RECT+16] = rect
    print(f"  srcRect at 0x{OFF_SRC_RECT:X}: {{0, 0, {VIRT_W}, {VIRT_H}}}")

    # 5. destRect: {320, 0, 2240, 1440}
    rect = make_rect(BAR_L, 0, BAR_R, DISP_H)
    data[OFF_DST_RECT:OFF_DST_RECT+16] = rect
    print(f"  destRect at 0x{OFF_DST_RECT:X}: {{{BAR_L}, 0, {BAR_R}, {DISP_H}}}")

    # 6. Scale cave code
    cave = build_cave_code()
    data[OFF_CAVE_CODE:OFF_CAVE_CODE+len(cave)] = cave
    print(f"  Cave code at 0x{OFF_CAVE_CODE:X} ({len(cave)} bytes)")

    # 7. Frontend Flip trampoline: CALL cave + 5 NOPs
    tramp = make_call_rel32(0x414D09, 0x45C330) + b'\x90' * 5
    data[OFF_FRONTEND_JMP:OFF_FRONTEND_JMP+10] = tramp
    print(f"  Frontend trampoline at 0x{OFF_FRONTEND_JMP:X}: CALL 0x45C330 + NOP*5")

    # 8. Loading screen Flip trampoline: CALL cave + 5 NOPs
    tramp = make_call_rel32(0x42CBBE, 0x45C330) + b'\x90' * 5
    data[OFF_LOADING_JMP:OFF_LOADING_JMP+10] = tramp
    print(f"  Loading trampoline at 0x{OFF_LOADING_JMP:X}: CALL 0x45C330 + NOP*5")

    # --- Write patched EXE ---
    with open(EXE, 'wb') as f:
        f.write(data)
    print(f"\nWrote patched EXE ({len(data)} bytes)")

    # --- Verify ---
    print("\nVerifying patches...")
    with open(EXE, 'rb') as f:
        verify = f.read()

    checks = [
        (OFF_DDBLTFX, b'\x64', "DDBLTFX dwSize"),
        (OFF_SRC_RECT, make_rect(0, 0, 640, 480), "srcRect"),
        (OFF_DST_RECT, make_rect(BAR_L, 0, BAR_R, DISP_H), "destRect"),
        (OFF_CAVE_CODE, b'\x60\x8B\x0D', "Cave start (PUSHAD+MOV)"),
        (OFF_CAVE_CODE + len(cave) - 1, b'\xC3', "Cave end (RET)"),
        (OFF_FRONTEND_JMP, b'\xE8', "Frontend CALL"),
        (OFF_LOADING_JMP, b'\xE8', "Loading CALL"),
    ]
    all_ok = True
    for off, expected, label in checks:
        actual = verify[off:off+len(expected)]
        status = "OK" if actual == expected else "FAIL"
        if status == "FAIL":
            all_ok = False
        print(f"  {label}: {status}")

    if all_ok:
        print("\nAll patches verified successfully!")
        print(f"\nScale cave: {len(cave)} bytes at VA 0x45C330")
        print(f"Pillarbox: {BAR_L}px bars on each side ({CONTENT_W}x{DISP_H} 4:3 centered)")
        print(f"Flip sites patched: frontend (0x414D0A) + loading (0x42CBBF)")
    else:
        print("\nSome verifications FAILED!")


if __name__ == '__main__':
    main()
