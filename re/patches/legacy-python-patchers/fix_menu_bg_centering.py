"""
fix_menu_bg_centering.py

Fixes main menu background centering at 2560x1440 for TD5_d3d.exe.

FUN_00424af0 draws the menu background by calling FUN_004251a0 (BltFast
wrapper) with destX=0, destY=0. dgVoodoo2's BltFast appears to ignore
destX/destY entirely, so nudging those params has no effect.

Fix: intercept the CALL to FUN_004251a0 at VA 0x424B21 (inside FUN_00424af0)
and redirect to a code cave that, when game_state==1 (menu), calls
IDirectDrawSurface::Blt (vtable+0x14) directly with a centered destRect.
Blt respects the destination rect coordinates (confirmed by working COLORFILL
centering patch).

Layout:
  Intercept VA 0x424B21:  5-byte CALL -> 5-byte JMP to CODE_VA
  Data      VA 0x45C250:  dest_rect {960,480,1600,960} + src_rect {0,0,640,480}
  Code      VA 0x45C270:  CMP state==1; menu->Blt; else->original CALL
"""

import struct, os

EXE        = "D:/Descargas/Test Drive 5 ISO/TD5_d3d.exe"
IMAGE_BASE = 0x400000

TARGET_W, TARGET_H = 2560, 1440
ORIG_W,   ORIG_H   = 640, 480
OFF_X = (TARGET_W - ORIG_W) // 2   # 960 = 0x3C0
OFF_Y = (TARGET_H - ORIG_H) // 2   # 480 = 0x1E0

GAME_STATE_VA  = 0x4C3CE8
BACK_BUF_VA    = 0x495220   # IDirectDrawSurface** (double-deref to get surface)
SRC_SURF_VA    = 0x496260   # IDirectDrawSurface*  (offscreen bg surface)

INTERCEPT_VA   = 0x424B21   # 5-byte CALL FUN_004251a0: E8 7A 06 00 00
RETURN_VA      = 0x424B26   # next insn after the CALL (ADD ESP, 0x24)
TARGET_FUNC_VA = 0x4251A0   # FUN_004251a0 (original BltFast wrapper)

DATA_VA        = 0x45C250   # dest_rect (16 bytes) + src_rect (16 bytes)
CODE_VA        = 0x45C270   # code cave (~56 bytes)

# Section ends at ~0x45D000; BltFast trampoline ends at 0x45C24A — plenty of room.

fo = lambda va: va - IMAGE_BASE

def u32(x): return struct.pack('<I', x)
def rel32(from_va, to_va): return struct.pack('<i', to_va - (from_va + 5))


def build_data():
    """32 bytes of static RECT data placed before the code cave."""
    dest = struct.pack('<4i', OFF_X, OFF_Y, OFF_X + ORIG_W, OFF_Y + ORIG_H)
    src  = struct.pack('<4i', 0, 0, ORIG_W, ORIG_H)
    assert len(dest) == 16 and len(src) == 16
    return dest + src


def build_code():
    """
    Code cave at CODE_VA.

    When game_state == 1 (menu):
      Load back-buffer surface (DAT_00495220 = IDirectDrawSurface**)
      Load vtable
      Call IDirectDrawSurface::Blt(destRect, srcSurf, srcRect, 0, NULL)
        vtable+0x14, __stdcall (callee cleans 5 args = 20 bytes)
      JMP RETURN_VA  (ADD ESP,0x24 in FUN_00424af0 cleans caller's pushed args)

    Otherwise:
      CALL original FUN_004251a0  (picks up the already-pushed args on stack)
      JMP RETURN_VA
    """
    c   = bytearray()
    va  = CODE_VA   # tracks current instruction VA in step with len(c)

    src_rect_va  = DATA_VA + 16
    dest_rect_va = DATA_VA

    # CMP DWORD [GAME_STATE_VA], 1         83 3D xx xx xx xx 01   7 bytes
    c += b'\x83\x3D' + u32(GAME_STATE_VA) + b'\x01'
    va += 7

    # JNE .skip                            75 ??                   2 bytes
    jne_off = len(c)
    c += b'\x75\x00'
    va += 2

    # --- menu path ---
    # Ghidra confirmed (FUN_004251a0 / FUN_00424050):
    #   DAT_00495220 = IDirectDrawSurface* (single ptr, NOT double-ptr)
    #   vtable = *(IDirectDrawSurface*) = first field of the COM object
    #   ABI: (*vtable[slot])(this, arg1, ...) — this pushed explicitly, callee cleans all

    # MOV EAX, [BACK_BUF_VA]              A1 xx xx xx xx          5 bytes
    # EAX = IDirectDrawSurface* (back-buffer surface = this for the call)
    c += b'\xA1' + u32(BACK_BUF_VA); va += 5

    # MOV ECX, [EAX]                       8B 08                   2 bytes
    # ECX = vtable pointer (first field of the COM object)
    c += b'\x8B\x08'; va += 2

    # Push Blt args right-to-left:
    # Signature: Blt(this, lpDestRect, lpDDSrcSurface, lpSrcRect, dwFlags, lpDDBltFx)
    # PUSH 0   ; lpDDBltFx = NULL          6A 00                   2 bytes  [arg 6, first push]
    c += b'\x6A\x00'; va += 2

    # PUSH 0   ; dwFlags = 0               6A 00                   2 bytes  [arg 5]
    c += b'\x6A\x00'; va += 2

    # PUSH src_rect_va                     68 xx xx xx xx          5 bytes  [arg 4]
    c += b'\x68' + u32(src_rect_va); va += 5

    # MOV EDX, [SRC_SURF_VA]              8B 15 xx xx xx xx       6 bytes
    # EDX = offscreen bg surface ptr; use EDX not EAX to preserve EAX = this
    c += b'\x8B\x15' + u32(SRC_SURF_VA); va += 6

    # PUSH EDX  ; lpDDSrcSurface           52                      1 byte   [arg 3]
    c += b'\x52'; va += 1

    # PUSH dest_rect_va                    68 xx xx xx xx          5 bytes  [arg 2]
    c += b'\x68' + u32(dest_rect_va); va += 5

    # PUSH EAX  ; this = back-buffer       50                      1 byte   [arg 1, last push]
    c += b'\x50'; va += 1

    # CALL [ECX+0x14]  ; Blt              FF 51 14                3 bytes
    # ECX = vtable; callee (__stdcall) pops 6*4=24 bytes via RET 0x18
    c += b'\xFF\x51\x14'; va += 3

    # JMP RETURN_VA                        E9 xx xx xx xx          5 bytes
    c += b'\xE9' + rel32(va, RETURN_VA); va += 5

    # --- .skip path ---
    skip_off = len(c)
    c[jne_off + 1] = skip_off - (jne_off + 2)   # fix JNE rel8

    # CALL TARGET_FUNC_VA                  E8 xx xx xx xx          5 bytes
    # Stack already has the 5 args pushed by FUN_00424af0, so this is valid
    c += b'\xE8' + rel32(va, TARGET_FUNC_VA); va += 5

    # JMP RETURN_VA                        E9 xx xx xx xx          5 bytes
    c += b'\xE9' + rel32(va, RETURN_VA); va += 5

    assert va == CODE_VA + len(c)
    return bytes(c)


def apply():
    data = build_data()
    code = build_code()

    print(f"dest_rect = {{{OFF_X}, {OFF_Y}, {OFF_X+ORIG_W}, {OFF_Y+ORIG_H}}}")
    print(f"src_rect  = {{0, 0, {ORIG_W}, {ORIG_H}}}")
    print(f"Data ({len(data)}B) @ VA 0x{DATA_VA:08X}: {data.hex(' ')}")
    print(f"Code ({len(code)}B) @ VA 0x{CODE_VA:08X}:")
    print(f"  {code.hex(' ')}")

    with open(EXE, 'rb') as f:
        exe = bytearray(f.read())

    # --- verify intercept site ---
    site = bytes(exe[fo(INTERCEPT_VA):fo(INTERCEPT_VA)+5])
    expected_call = b'\xE8' + rel32(INTERCEPT_VA, TARGET_FUNC_VA)
    expected_jmp  = b'\xE9' + rel32(INTERCEPT_VA, CODE_VA)

    refix = site == expected_jmp   # JMP already in place — just rewrite the code cave
    if refix:
        print("JMP already at intercept site — rewriting code cave only.")
    if not refix and site != expected_call:
        print(f"WARNING: intercept site bytes = {site.hex(' ')}")
        print(f"         expected CALL         = {expected_call.hex(' ')}")
        if site[0] == 0xE9:
            print("JMP present but not ours — aborting.")
            return
        print("Proceeding despite mismatch...")

    # --- verify cave area is free (skip if refixing) ---
    if not refix:
        cave_span = bytes(exe[fo(DATA_VA):fo(CODE_VA) + len(code)])
        if any(b != 0 for b in cave_span):
            print(f"WARNING: cave area not all-zero: {cave_span[:48].hex(' ')}")
            print("Proceeding anyway.")

    # --- apply ---
    exe[fo(DATA_VA):fo(DATA_VA) + len(data)] = data
    exe[fo(CODE_VA):fo(CODE_VA) + len(code)] = code

    if not refix:
        jmp = b'\xE9' + rel32(INTERCEPT_VA, CODE_VA)
        exe[fo(INTERCEPT_VA):fo(INTERCEPT_VA)+5] = jmp
        print(f"Wrote JMP at 0x{INTERCEPT_VA:08X} (file 0x{fo(INTERCEPT_VA):06X}): {jmp.hex(' ')}")

    with open(EXE, 'wb') as f:
        f.write(exe)
    print("Done.")


def verify():
    data = build_data()
    code = build_code()
    with open(EXE, 'rb') as f:
        exe = f.read()

    expected_jmp = b'\xE9' + rel32(INTERCEPT_VA, CODE_VA)
    site = exe[fo(INTERCEPT_VA):fo(INTERCEPT_VA)+5]
    print(f"Intercept site: {site.hex(' ')}")
    print(f"Expected JMP:   {expected_jmp.hex(' ')}  {'OK' if site == expected_jmp else 'MISMATCH'}")

    actual_data = exe[fo(DATA_VA):fo(DATA_VA) + len(data)]
    print(f"\nData: {actual_data.hex(' ')}")
    print(f"Exp:  {data.hex(' ')}  {'OK' if actual_data == data else 'MISMATCH'}")

    actual_code = exe[fo(CODE_VA):fo(CODE_VA) + len(code)]
    print(f"\nCode: {actual_code.hex(' ')}")
    print(f"Exp:  {code.hex(' ')}  {'OK' if actual_code == code else 'MISMATCH'}")


if __name__ == "__main__":
    import sys
    if "--verify" in sys.argv:
        verify()
    else:
        apply()
