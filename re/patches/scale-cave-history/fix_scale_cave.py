"""
Fix scale cave: Blt back->front (cross-surface, no overlap) instead of same-surface.
Also removes the Flip call — Blt-to-primary IS the presentation with dgVoodoo2.
"""

import struct
import os

EXE = r"D:\Descargas\Test Drive 5 ISO\TD5_d3d.exe"

def build_cave_v2():
    """
    Scale cave v2 at VA 0x45C330:
    1. PUSHAD
    2. Get front surface (ESI) and back surface (EBX) from dd_exref
    3. Blt stretch: BACK {0,0,640,480} -> FRONT {320,0,2240,1440}  (cross-surface!)
    4. COLORFILL left bar on FRONT
    5. COLORFILL right bar on FRONT
    6. POPAD + RET  (NO Flip — Blt-to-primary presents via dgVoodoo2)
    """
    code = bytearray()

    # PUSHAD
    code += b'\x60'

    # MOV ECX, [0x45D564]  (dd_exref)
    code += b'\x8B\x0D' + struct.pack('<I', 0x45D564)

    # MOV ESI, [ECX+4]  (front/primary surface)
    code += b'\x8B\x71\x04'

    # MOV EBX, [ECX+8]  (back/render target surface)
    code += b'\x8B\x59\x08'

    # --- Blt stretch: back -> front (cross-surface, no overlap!) ---
    code += b'\x6A\x00'                                    # PUSH 0 (lpDDBltFx)
    code += b'\x68' + struct.pack('<I', 0x01000000)        # PUSH DDBLT_WAIT
    code += b'\x68' + struct.pack('<I', 0x45C310)          # PUSH srcRect
    code += b'\x53'                                        # PUSH EBX (source = back)
    code += b'\x68' + struct.pack('<I', 0x45C320)          # PUSH destRect
    code += b'\x56'                                        # PUSH ESI (dest = front)
    code += b'\x8B\x0E'                                    # MOV ECX, [ESI] (vtable)
    code += b'\xFF\x51\x14'                                # CALL [ECX+0x14] (Blt)

    # --- COLORFILL left bar on FRONT ---
    code += b'\x68' + struct.pack('<I', 0x45C1A1)          # PUSH ddbltfx
    code += b'\x68' + struct.pack('<I', 0x01000400)        # PUSH DDBLT_COLORFILL|DDBLT_WAIT
    code += b'\x6A\x00'                                    # PUSH 0 (srcRect)
    code += b'\x6A\x00'                                    # PUSH 0 (srcSurf)
    code += b'\x68' + struct.pack('<I', 0x45C205)          # PUSH left bar rect
    code += b'\x56'                                        # PUSH ESI (front)
    code += b'\x8B\x0E'                                    # MOV ECX, [ESI] (vtable)
    code += b'\xFF\x51\x14'                                # CALL [ECX+0x14] (Blt)

    # --- COLORFILL right bar on FRONT ---
    code += b'\x68' + struct.pack('<I', 0x45C1A1)          # PUSH ddbltfx
    code += b'\x68' + struct.pack('<I', 0x01000400)        # PUSH DDBLT_COLORFILL|DDBLT_WAIT
    code += b'\x6A\x00'                                    # PUSH 0 (srcRect)
    code += b'\x6A\x00'                                    # PUSH 0 (srcSurf)
    code += b'\x68' + struct.pack('<I', 0x45C215)          # PUSH right bar rect
    code += b'\x56'                                        # PUSH ESI (front)
    code += b'\x8B\x0E'                                    # MOV ECX, [ESI] (vtable)
    code += b'\xFF\x51\x14'                                # CALL [ECX+0x14] (Blt)

    # --- POPAD + RET (no Flip!) ---
    code += b'\x61'
    code += b'\xC3'

    return bytes(code)


def main():
    print("Fix scale cave: back->front cross-surface Blt")
    print("=" * 50)

    with open(EXE, 'rb') as f:
        data = bytearray(f.read())

    cave = build_cave_v2()
    print(f"Cave v2: {len(cave)} bytes")

    # Verify the trampoline CALLs are already in place
    assert data[0x14D09] == 0xE8, "Frontend trampoline not found"
    assert data[0x2CBBE] == 0xE8, "Loading trampoline not found"

    # Overwrite cave code (zero out old, write new)
    old_cave_start = 0x5C330
    # Zero out old cave area (97 bytes)
    for i in range(97):
        data[old_cave_start + i] = 0x00
    # Write new cave
    data[old_cave_start:old_cave_start + len(cave)] = cave

    with open(EXE, 'wb') as f:
        f.write(data)

    # Verify
    with open(EXE, 'rb') as f:
        v = f.read()
    assert v[old_cave_start] == 0x60, "Cave start (PUSHAD)"
    assert v[old_cave_start + len(cave) - 1] == 0xC3, "Cave end (RET)"
    # Check ESI usage (front surface) instead of old EBX-only
    assert v[old_cave_start + 7] == 0x8B and v[old_cave_start + 8] == 0x71, "MOV ESI, [ECX+4]"

    print("Verified OK")
    print()
    print("Changes from v1:")
    print("  - Blt dest = FRONT surface (cross-surface, no overlap)")
    print("  - COLORFILL on FRONT surface")
    print("  - NO Flip call (Blt-to-primary presents via dgVoodoo2)")
    print()
    print("Restart game to test (game + debugger must be closed first)")


if __name__ == '__main__':
    main()
