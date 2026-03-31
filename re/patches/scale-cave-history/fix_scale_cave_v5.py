"""
Scale cave v5: No overlap, proper VSync.
1. Copy back {0,0,640,480} -> front {0,0,640,480}  (snapshot)
2. COLORFILL entire back to black                    (clear everything)
3. Stretch front {0,0,640,480} -> back {320,0,2240,1440}  (cross-surface)
4. Flip                                              (VSync + present back)
"""
import struct
EXE = r"D:\Descargas\Test Drive 5 ISO\TD5_d3d.exe"

def build_cave():
    code = bytearray()
    code += b'\x60'                                        # PUSHAD
    code += b'\x8B\x0D' + struct.pack('<I', 0x45D564)     # MOV ECX, [dd_exref]
    code += b'\x8B\x71\x04'                                # MOV ESI, [ECX+4] front
    code += b'\x8B\x59\x08'                                # MOV EBX, [ECX+8] back

    # Step 1: Copy back {0,0,640,480} -> front {0,0,640,480}
    code += b'\x6A\x00'                                    # PUSH 0 (ddbltfx)
    code += b'\x68' + struct.pack('<I', 0x01000000)        # PUSH DDBLT_WAIT
    code += b'\x68' + struct.pack('<I', 0x45C310)          # PUSH srcRect
    code += b'\x53'                                        # PUSH EBX (back)
    code += b'\x68' + struct.pack('<I', 0x45C310)          # PUSH destRect (same=1:1)
    code += b'\x56'                                        # PUSH ESI (front)
    code += b'\x8B\x0E'                                    # MOV ECX, [ESI]
    code += b'\xFF\x51\x14'                                # CALL Blt

    # Step 2: COLORFILL entire back to black (destRect=NULL=whole surface)
    code += b'\x68' + struct.pack('<I', 0x45C1A1)          # PUSH ddbltfx (black)
    code += b'\x68' + struct.pack('<I', 0x01000400)        # PUSH COLORFILL|WAIT
    code += b'\x6A\x00'                                    # PUSH 0 (srcRect)
    code += b'\x6A\x00'                                    # PUSH 0 (srcSurf)
    code += b'\x6A\x00'                                    # PUSH 0 (destRect=NULL=entire)
    code += b'\x53'                                        # PUSH EBX (back)
    code += b'\x8B\x0B'                                    # MOV ECX, [EBX]
    code += b'\xFF\x51\x14'                                # CALL Blt

    # Step 3: Stretch front {0,0,640,480} -> back {320,0,2240,1440}
    code += b'\x6A\x00'                                    # PUSH 0 (ddbltfx)
    code += b'\x68' + struct.pack('<I', 0x01000000)        # PUSH DDBLT_WAIT
    code += b'\x68' + struct.pack('<I', 0x45C310)          # PUSH srcRect
    code += b'\x56'                                        # PUSH ESI (front)
    code += b'\x68' + struct.pack('<I', 0x45C320)          # PUSH destRect
    code += b'\x53'                                        # PUSH EBX (back)
    code += b'\x8B\x0B'                                    # MOV ECX, [EBX]
    code += b'\xFF\x51\x14'                                # CALL Blt

    code += b'\x61'                                        # POPAD

    # Step 4: DXDraw::Flip(1) for VSync
    code += b'\x6A\x01'                                    # PUSH 1
    code += b'\xFF\x15' + struct.pack('<I', 0x45D504)      # CALL [Flip]
    code += b'\x83\xC4\x04'                                # ADD ESP, 4
    code += b'\xC3'                                        # RET
    return bytes(code)

with open(EXE, 'rb') as f:
    data = bytearray(f.read())

cave = build_cave()
print(f"Cave v5: {len(cave)} bytes")

for i in range(150):
    data[0x5C330 + i] = 0
data[0x5C330:0x5C330+len(cave)] = cave

with open(EXE, 'wb') as f:
    f.write(data)
print("Applied.")
print("Flow: snapshot back->front | clear back | stretch front->back | Flip")
print("All cross-surface, no overlap, proper VSync.")
