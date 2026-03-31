"""
Scale cave v4: Same-surface Blt stretch on back buffer + Flip.
Simplest possible approach - relies on dgVoodoo2 handling overlapping same-surface Blt correctly.
"""
import struct

EXE = r"D:\Descargas\Test Drive 5 ISO\TD5_d3d.exe"

def build_cave_v4():
    code = bytearray()
    code += b'\x60'                                        # PUSHAD
    code += b'\x8B\x0D' + struct.pack('<I', 0x45D564)     # MOV ECX, [dd_exref]
    code += b'\x8B\x59\x08'                                # MOV EBX, [ECX+8] (back)

    # Blt stretch: back {0,0,640,480} -> back {320,0,2240,1440}
    code += b'\x6A\x00'                                    # PUSH 0
    code += b'\x68' + struct.pack('<I', 0x01000000)        # PUSH DDBLT_WAIT
    code += b'\x68' + struct.pack('<I', 0x45C310)          # PUSH srcRect
    code += b'\x53'                                        # PUSH EBX (source=back)
    code += b'\x68' + struct.pack('<I', 0x45C320)          # PUSH destRect
    code += b'\x53'                                        # PUSH EBX (dest=back)
    code += b'\x8B\x0B'                                    # MOV ECX, [EBX]
    code += b'\xFF\x51\x14'                                # CALL [ECX+0x14]

    # COLORFILL left bar
    code += b'\x68' + struct.pack('<I', 0x45C1A1)
    code += b'\x68' + struct.pack('<I', 0x01000400)
    code += b'\x6A\x00'
    code += b'\x6A\x00'
    code += b'\x68' + struct.pack('<I', 0x45C205)
    code += b'\x53'
    code += b'\x8B\x0B'
    code += b'\xFF\x51\x14'

    # COLORFILL right bar
    code += b'\x68' + struct.pack('<I', 0x45C1A1)
    code += b'\x68' + struct.pack('<I', 0x01000400)
    code += b'\x6A\x00'
    code += b'\x6A\x00'
    code += b'\x68' + struct.pack('<I', 0x45C215)
    code += b'\x53'
    code += b'\x8B\x0B'
    code += b'\xFF\x51\x14'

    code += b'\x61'                                        # POPAD
    code += b'\x6A\x01'                                    # PUSH 1
    code += b'\xFF\x15' + struct.pack('<I', 0x45D504)      # CALL [DXDraw::Flip]
    code += b'\x83\xC4\x04'                                # ADD ESP, 4
    code += b'\xC3'                                        # RET
    return bytes(code)

with open(EXE, 'rb') as f:
    data = bytearray(f.read())

cave = build_cave_v4()
print(f"Cave v4: {len(cave)} bytes (same-surface + Flip)")

# Zero old cave, write new
for i in range(150):
    data[0x5C330 + i] = 0
data[0x5C330:0x5C330+len(cave)] = cave

with open(EXE, 'wb') as f:
    f.write(data)
print("Applied. Restart game to test.")
