"""
Scale cave v2+vsync: cross-surface Blt back->front + COLORFILL + busy-wait 16ms frame cap.
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

    # --- Busy-wait ~16ms frame cap using timeGetTime ---
    # CALL timeGetTime -> EAX = start time
    code += b'\xFF\x15' + struct.pack('<I', 0x45D5C0)  # CALL [timeGetTime]
    code += b'\x8B\xF8'                                 # MOV EDI, EAX (save start)
    # loop:
    loop_start = len(code)
    code += b'\xFF\x15' + struct.pack('<I', 0x45D5C0)  # CALL [timeGetTime]
    code += b'\x2B\xC7'                                 # SUB EAX, EDI (elapsed)
    code += b'\x83\xF8\x10'                             # CMP EAX, 16 (ms)
    # JB loop (jump back if < 16ms)
    jb_offset = loop_start - (len(code) + 2)            # relative offset
    code += b'\x72' + struct.pack('b', jb_offset)       # JB rel8

    # POPAD + RET
    code += b'\x61'
    code += b'\xC3'
    return bytes(code)

with open(EXE, 'rb') as f:
    data = bytearray(f.read())

cave = build_cave()
print(f"Cave v2+vsync: {len(cave)} bytes")

for i in range(150):
    data[0x5C330 + i] = 0
data[0x5C330:0x5C330+len(cave)] = cave

with open(EXE, 'wb') as f:
    f.write(data)
print("Applied. Back->front Blt + COLORFILL + 16ms busy-wait frame cap.")
