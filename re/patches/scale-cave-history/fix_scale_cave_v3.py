"""
Scale cave v3: Two-step cross-surface Blt + Flip
1. Copy back {0,0,640,480} -> front {0,0,640,480}  (temp snapshot, cross-surface)
2. Stretch front {0,0,640,480} -> back {320,0,2240,1440}  (cross-surface, no overlap!)
3. COLORFILL left/right bars on back
4. Flip (triggers dgVoodoo2 Present)
"""

import struct
import os

EXE = r"D:\Descargas\Test Drive 5 ISO\TD5_d3d.exe"


def build_cave_v3():
    code = bytearray()

    # PUSHAD
    code += b'\x60'

    # MOV ECX, [0x45D564]  (dd_exref)
    code += b'\x8B\x0D' + struct.pack('<I', 0x45D564)
    # MOV ESI, [ECX+4]  (front surface)
    code += b'\x8B\x71\x04'
    # MOV EBX, [ECX+8]  (back surface)
    code += b'\x8B\x59\x08'

    # --- Step 1: Copy back -> front (unscaled snapshot) ---
    # Blt(front, srcRect, back, srcRect, WAIT, NULL)  -- same rect = 1:1 copy
    code += b'\x6A\x00'                                    # PUSH 0 (lpDDBltFx)
    code += b'\x68' + struct.pack('<I', 0x01000000)        # PUSH DDBLT_WAIT
    code += b'\x68' + struct.pack('<I', 0x45C310)          # PUSH srcRect {0,0,640,480}
    code += b'\x53'                                        # PUSH EBX (source = back)
    code += b'\x68' + struct.pack('<I', 0x45C310)          # PUSH destRect {0,0,640,480} (same)
    code += b'\x56'                                        # PUSH ESI (dest = front)
    code += b'\x8B\x0E'                                    # MOV ECX, [ESI]
    code += b'\xFF\x51\x14'                                # CALL [ECX+0x14] (Blt)

    # --- Step 2: Stretch front -> back (cross-surface, no overlap!) ---
    # Blt(back, destRect, front, srcRect, WAIT, NULL)
    code += b'\x6A\x00'                                    # PUSH 0 (lpDDBltFx)
    code += b'\x68' + struct.pack('<I', 0x01000000)        # PUSH DDBLT_WAIT
    code += b'\x68' + struct.pack('<I', 0x45C310)          # PUSH srcRect {0,0,640,480}
    code += b'\x56'                                        # PUSH ESI (source = front)
    code += b'\x68' + struct.pack('<I', 0x45C320)          # PUSH destRect {320,0,2240,1440}
    code += b'\x53'                                        # PUSH EBX (dest = back)
    code += b'\x8B\x0B'                                    # MOV ECX, [EBX]
    code += b'\xFF\x51\x14'                                # CALL [ECX+0x14] (Blt)

    # --- Step 3: COLORFILL left bar on back ---
    code += b'\x68' + struct.pack('<I', 0x45C1A1)          # PUSH ddbltfx
    code += b'\x68' + struct.pack('<I', 0x01000400)        # PUSH DDBLT_COLORFILL|WAIT
    code += b'\x6A\x00'                                    # PUSH 0
    code += b'\x6A\x00'                                    # PUSH 0
    code += b'\x68' + struct.pack('<I', 0x45C205)          # PUSH left bar rect
    code += b'\x53'                                        # PUSH EBX
    code += b'\x8B\x0B'                                    # MOV ECX, [EBX]
    code += b'\xFF\x51\x14'                                # CALL [ECX+0x14]

    # --- Step 4: COLORFILL right bar on back ---
    code += b'\x68' + struct.pack('<I', 0x45C1A1)          # PUSH ddbltfx
    code += b'\x68' + struct.pack('<I', 0x01000400)        # PUSH DDBLT_COLORFILL|WAIT
    code += b'\x6A\x00'                                    # PUSH 0
    code += b'\x6A\x00'                                    # PUSH 0
    code += b'\x68' + struct.pack('<I', 0x45C215)          # PUSH right bar rect
    code += b'\x53'                                        # PUSH EBX
    code += b'\x8B\x0B'                                    # MOV ECX, [EBX]
    code += b'\xFF\x51\x14'                                # CALL [ECX+0x14]

    # --- POPAD ---
    code += b'\x61'

    # --- Step 5: DXDraw::Flip(1) ---
    code += b'\x6A\x01'                                    # PUSH 1
    code += b'\xFF\x15' + struct.pack('<I', 0x45D504)      # CALL [0x45D504]
    code += b'\x83\xC4\x04'                                # ADD ESP, 4

    # --- RET ---
    code += b'\xC3'

    return bytes(code)


def main():
    print("Scale cave v3: two-step cross-surface Blt + Flip")
    print("=" * 50)

    with open(EXE, 'rb') as f:
        data = bytearray(f.read())

    cave = build_cave_v3()
    print(f"Cave v3: {len(cave)} bytes")

    # Verify trampolines still in place
    assert data[0x14D09] == 0xE8, "Frontend trampoline missing"
    assert data[0x2CBBE] == 0xE8, "Loading trampoline missing"

    # Zero out old cave (generous range)
    for i in range(150):
        data[0x5C330 + i] = 0x00

    # Write new cave
    data[0x5C330:0x5C330 + len(cave)] = cave

    with open(EXE, 'wb') as f:
        f.write(data)

    # Verify
    with open(EXE, 'rb') as f:
        v = f.read()
    assert v[0x5C330] == 0x60, "PUSHAD"
    assert v[0x5C330 + len(cave) - 1] == 0xC3, "RET"

    print("Verified OK")
    print()
    print("Flow: copy back->front | stretch front->back | COLORFILL bars | Flip")
    print("All Blts cross-surface (no overlap), Flip triggers dgVoodoo2 Present")


if __name__ == '__main__':
    main()
