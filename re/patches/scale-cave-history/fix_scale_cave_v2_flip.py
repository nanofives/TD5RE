"""
Scale cave v2+flip: cross-surface Blt back->front + COLORFILL + Flip for VSync.
No busy-wait. Flip provides proper VSync timing via DDrawCompat.
"""
import struct
EXE = r"D:\Descargas\Test Drive 5 ISO\TD5_d3d.exe"

def build_cave():
    code = bytearray()
    # PUSHAD
    code += b'\x60'
    # MOV ECX, [dd_exref]
    code += b'\x8B\x0D' + struct.pack('<I', 0x45D564)
    # MOV ESI, [ECX+4] (front)
    code += b'\x8B\x71\x04'
    # MOV EBX, [ECX+8] (back)
    code += b'\x8B\x59\x08'

    # --- Blt stretch: back -> front ---
    code += b'\x6A\x00'
    code += b'\x68' + struct.pack('<I', 0x01000000)
    code += b'\x68' + struct.pack('<I', 0x45C310)  # srcRect
    code += b'\x53'                                  # back
    code += b'\x68' + struct.pack('<I', 0x45C320)  # destRect
    code += b'\x56'                                  # front
    code += b'\x8B\x0E'
    code += b'\xFF\x51\x14'

    # --- COLORFILL left bar on front ---
    code += b'\x68' + struct.pack('<I', 0x45C1A1)
    code += b'\x68' + struct.pack('<I', 0x01000400)
    code += b'\x6A\x00'
    code += b'\x6A\x00'
    code += b'\x68' + struct.pack('<I', 0x45C205)
    code += b'\x56'
    code += b'\x8B\x0E'
    code += b'\xFF\x51\x14'

    # --- COLORFILL right bar on front ---
    code += b'\x68' + struct.pack('<I', 0x45C1A1)
    code += b'\x68' + struct.pack('<I', 0x01000400)
    code += b'\x6A\x00'
    code += b'\x6A\x00'
    code += b'\x68' + struct.pack('<I', 0x45C215)
    code += b'\x56'
    code += b'\x8B\x0E'
    code += b'\xFF\x51\x14'

    # POPAD
    code += b'\x61'

    # --- DXDraw::Flip(1) for VSync ---
    code += b'\x6A\x01'                                    # PUSH 1
    code += b'\xFF\x15' + struct.pack('<I', 0x45D504)      # CALL [DXDraw::Flip]
    code += b'\x83\xC4\x04'                                # ADD ESP, 4

    # RET
    code += b'\xC3'
    return bytes(code)

with open(EXE, 'rb') as f:
    data = bytearray(f.read())

cave = build_cave()
print(f"Cave v2+flip: {len(cave)} bytes")

for i in range(150):
    data[0x5C330 + i] = 0
data[0x5C330:0x5C330+len(cave)] = cave

with open(EXE, 'wb') as f:
    f.write(data)
print("Applied. Back->front Blt + COLORFILL + Flip (VSync).")
