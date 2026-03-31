"""
Scale cave v6: Blt back->front + COLORFILL on front + WaitForVerticalBlank.
No Flip (avoids DXDraw::Flip's internal Blt that causes double rendering).
VSync via IDirectDraw7::WaitForVerticalBlank directly.
"""
import struct
EXE = r"D:\Descargas\Test Drive 5 ISO\TD5_d3d.exe"

def build_cave():
    code = bytearray()
    code += b'\x60'                                        # PUSHAD
    code += b'\x8B\x0D' + struct.pack('<I', 0x45D564)     # MOV ECX, [dd_exref]
    code += b'\x8B\x71\x04'                                # MOV ESI, [ECX+4] front
    code += b'\x8B\x59\x08'                                # MOV EBX, [ECX+8] back

    # --- Blt stretch: back -> front ---
    code += b'\x6A\x00'
    code += b'\x68' + struct.pack('<I', 0x01000000)
    code += b'\x68' + struct.pack('<I', 0x45C310)          # srcRect
    code += b'\x53'                                        # back
    code += b'\x68' + struct.pack('<I', 0x45C320)          # destRect
    code += b'\x56'                                        # front
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

    # --- WaitForVerticalBlank(DDWAITVB_BLOCKBEGIN, NULL) ---
    # IDirectDraw7* is at [dd_exref + 0]
    code += b'\x8B\x0D' + struct.pack('<I', 0x45D564)     # MOV ECX, [dd_exref]
    code += b'\x8B\x01'                                    # MOV EAX, [ECX] = IDirectDraw7*
    code += b'\x8B\x08'                                    # MOV ECX, [EAX] = vtable
    code += b'\x6A\x00'                                    # PUSH 0 (hEvent)
    code += b'\x6A\x01'                                    # PUSH 1 (DDWAITVB_BLOCKBEGIN)
    code += b'\x50'                                        # PUSH EAX (this)
    code += b'\xFF\x51\x58'                                # CALL [ECX+0x58] WaitForVerticalBlank

    # POPAD + RET
    code += b'\x61'
    code += b'\xC3'
    return bytes(code)

with open(EXE, 'rb') as f:
    data = bytearray(f.read())

cave = build_cave()
print(f"Cave v6: {len(cave)} bytes")

for i in range(150):
    data[0x5C330 + i] = 0
data[0x5C330:0x5C330+len(cave)] = cave

with open(EXE, 'wb') as f:
    f.write(data)
print("Applied.")
print("Flow: Blt back->front | COLORFILL bars on front | WaitForVerticalBlank")
print("No Flip, no double rendering, proper VSync.")
