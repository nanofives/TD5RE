"""
patch_scale_to_fullscreen.py

Scales the 640x480 menu render to fill 2560x1440 (pillar-boxed 3x, 4:3).

Strategy:
  1. Zero all existing centering offsets so menu content renders to {0,0,640,480}
     in the back buffer.
  2. Intercept BOTH Flip call sites in FUN_00414b50:
       Path A (DAT_0049523c != 0): CALL [0x45D504] at VA 0x414D0A -> cave_A (0x45C330)
       Path B (DAT_0049523c == 0): CALL [ECX+0x58]  at VA 0x414D79 -> cave_B (0x45C383)
  3. Pre-Flip, when game_state == 1 (menu/frontend) and pIntermediate != NULL:
       a. Blt back_buffer{0,0,640,480} -> pIntermediate (DAT_00496264) -- copy
       b. Blt pIntermediate{0,0,640,480} -> back_buffer{0,0,1920,1440} -- scale 3x
  4. Call original Flip (path-A: IAT; path-B: vtable+0x58 on DXDraw object).

Offsets zeroed:
  BltFast cave  0x45C229+0x0C : ADD [EBP+8], 960 immediate -> 0
  BltFast cave  0x45C229+0x13 : ADD [EBP+C], 480 immediate -> 0
  Blt bg data   0x45C250      : destRect {960,480,1600,960} -> {0,0,640,480}
  Car D3D cave  0x45C2F6+0x0D : ADD EAX, 0x258780 immediate -> 0

Pre-Flip cave layout:
  0x45C310  srcRect  {0,0,640,480}     16 bytes (data)
  0x45C320  destRect {0,0,1920,1440}   16 bytes (data)
  0x45C330  cave_A code                92 bytes  (path A intercept)
  0x45C38C  cave_B code                93 bytes  (path B intercept)
  0x45C3E9  cave_C code                92 bytes  (intercept 0x42CBBF)
  0x45C445  cave_D code                92 bytes  (intercept 0x442502)
  Intercept A at 0x414D0A: FF 15 04 D5 45 00 -> E9 xx xx xx xx 90
  Intercept B at 0x414D75: 6A 00 53 50 FF    -> E9 xx xx xx xx
              at 0x414D7A: 51 58              -> 90 90  (tail of destroyed CALL)

Pillar-box geometry:
  640 * 3 = 1920, 480 * 3 = 1440 (exact 4:3 fill at 3x)
  Horizontal margin: (2560 - 1920) / 2 = 320 px each side

Blt flags: 0 (dwFlags=0 works with dgVoodoo2; DDBLT_WAIT=0x01000000 is too wide
  for a sign-extended byte push and is not necessary in software mode).
"""

import struct

EXE        = "D:/Descargas/Test Drive 5 ISO/TD5_d3d.exe"
IMAGE_BASE = 0x400000

GAME_STATE_VA  = 0x4C3CE8
BACK_BUF_VA    = 0x495220   # IDirectDrawSurface* back-buffer (single ptr)
INTER_BUF_VA   = 0x496264   # IDirectDrawSurface* intermediate 640x480 buffer
FLIP_IAT_VA    = 0x45D504   # IAT slot: DXDraw::Flip

INTERCEPT_VA   = 0x414D0A   # CALL [0x45D504] 6 bytes: FF 15 04 D5 45 00  (path A)
RETURN_VA      = 0x414D10   # ADD ESP,4 — next instruction after path-A Flip CALL

INTERCEPT_B_VA = 0x414D75   # 6A 00 53 50 FF (5 bytes before CALL [ECX+0x58])    (path B)
RETURN_B_VA    = 0x414D7C   # instruction after CALL [ECX+0x58] (skip mangled 51 58)
DXDRAW_PTR_VA  = 0x45D564   # IAT/data: ptr to DXDraw object used by path B

SRCRECT_VA     = 0x45C310   # data: {0,0,640,480}
DESTRECT_VA    = 0x45C320   # data: {320,0,2240,1440}   pillar-box
FULLRECT_VA    = 0x45C37A   # UNUSED — kept as constant only; writing here would corrupt cave_A
CAVE_CODE_VA   = 0x45C330   # code (path A, FUN_00414b50) — 78 bytes, ends at 0x45C37E
CAVE_B_VA      = 0x45C330 + 92   # = 0x45C38C, code: 93 bytes (path B)
LEFT_BAR_RECT_VA  = 0x45C501 # {0,0,320,1440}   — left bar dest (cleared after scale blit)
BLACK_ROW_RECT_VA = 0x45C511 # {0,500,320,501}  — 1-row source of guaranteed-black pixels (y>480)

# Real Flip call sites (found by searching for FF 15 04 D5 45 00):
INTERCEPT_C_VA = 0x0042CBBF  # CALL [Flip IAT] inside software-render func (~race/menu area)
RETURN_C_VA    = 0x0042CBC5  # next instr after the 6-byte CALL: ADD ESP,4 / func epilogue
CAVE_C_VA      = 0x45C38C + 93   # = 0x45C3E9, code: 92 bytes (cave_B ends at 0x45C38C+93-1)

INTERCEPT_D_VA = 0x00442502  # CALL [Flip IAT] inside outer render/present function
RETURN_D_VA    = 0x00442508  # next instr: PUSH 0; CALL [0x45D4FC]; ADD ESP,8 ...
CAVE_D_VA      = 0x45C3E9 + 92   # = 0x45C445, code: 92 bytes

# Existing centering offset patch points
BLTFAST_ADD_X_VA = 0x45C229 + 0x0C   # 4-byte imm32: 960 = C0 03 00 00
BLTFAST_ADD_Y_VA = 0x45C229 + 0x13   # 4-byte imm32: 480 = E0 01 00 00
BLT_BG_DESTRECT  = 0x45C250          # 16 bytes: {960,480,1600,960}
CAR_ADD_IMME_VA  = 0x45C2F6 + 0x0D   # 4-byte imm32: 0x258780

ORIG_FLIP_CALL   = bytes([0xFF, 0x15, 0x04, 0xD5, 0x45, 0x00])

fo    = lambda va: va - IMAGE_BASE
u32   = lambda x: struct.pack('<I', x)
rel32 = lambda frm, to: struct.pack('<i', to - (frm + 5))


def build_rects():
    srcrect   = struct.pack('<4i', 0, 0, 640, 480)
    destrect  = struct.pack('<4i', 0, 0, 2560, 1440)    # TEST: full-surface stretch (eliminates all stale)
    fullrect  = struct.pack('<4i', 0, 0, 2560, 1440)    # full-surface clear
    return srcrect, destrect, fullrect


def build_cave_code():
    """
    Two-blit cave at CAVE_CODE_VA (74 bytes).

    Blt1 (scale):  backbuf{0,0,640,480}   → backbuf{320,0,2240,1440}  (pillar-box 3x)
    Blt2 (black):  backbuf{0,500,320,501} → backbuf{0,0,320,1440}     (erase left bar)

    After Blt1, the original small game render remains in {0,0,320,480} (dest starts
    at x=320 so x=0-319 is untouched).  Blt2 overwrites it by stretching a 1-row
    strip from y=500 — guaranteed black because the game only renders to y<480 and
    dgVoodoo2 initialises surfaces to zero.

    NOTE: Blt2 source {0,500,320,501} is outside Blt1 dest {320,0,2240,1440}
    (x<320 is never written by Blt1), so it is still black when Blt2 reads it.
    """
    c  = bytearray()
    va = CAVE_CODE_VA

    # CMP DWORD [GAME_STATE_VA], 1    83 3D xx xx xx xx 01    7 bytes
    c += b'\x83\x3D' + u32(GAME_STATE_VA) + b'\x01'; va += 7

    # JNE .do_flip                    75 ??                   2 bytes
    jne_off = len(c)
    c += b'\x75\x00'; va += 2

    # PUSHAD                          60                      1 byte
    c += b'\x60'; va += 1

    def blt26(src_rect_va, dest_rect_va):
        """Build 26-byte self-Blt block (dwFlags=0, lpDDBltFx=NULL)."""
        b  = b'\xA1' + u32(BACK_BUF_VA)   # MOV EAX,[BACK_BUF_VA]  5
        b += b'\x8B\x08'                   # MOV ECX,[EAX]           2
        b += b'\x6A\x00'                   # PUSH 0  lpDDBltFx       2
        b += b'\x6A\x00'                   # PUSH 0  dwFlags         2
        b += b'\x68' + u32(src_rect_va)    # PUSH lpSrcRect          5
        b += b'\x50'                       # PUSH EAX  lpSrcSurface  1
        b += b'\x68' + u32(dest_rect_va)   # PUSH lpDestRect         5
        b += b'\x50'                       # PUSH EAX  this          1
        b += b'\xFF\x51\x14'               # CALL [ECX+0x14]         3
        assert len(b) == 26
        return b

    # TEST Blt: scale 640×480 → full surface {0,0,2560,1440}
    # If this eliminates doubling, original render at {0,0,640,480} was the source.
    # If doubling persists, a second rendering path is writing outside our scale.
    c += blt26(SRCRECT_VA, DESTRECT_VA); va += 26

    # POPAD                           61                      1 byte
    c += b'\x61'; va += 1

    # .do_flip — fix JNE rel8
    do_flip_off = len(c)
    c[jne_off + 1] = do_flip_off - (jne_off + 2)

    # CALL [FLIP_IAT_VA]              FF 15 xx xx xx xx       6 bytes
    c += b'\xFF\x15' + u32(FLIP_IAT_VA); va += 6
    # JMP RETURN_VA                   E9 xx xx xx xx          5 bytes
    c += b'\xE9' + rel32(va, RETURN_VA); va += 5

    assert va == CAVE_CODE_VA + len(c), f"VA tracking mismatch: {va:08X} vs {CAVE_CODE_VA+len(c):08X}"
    assert len(c) == 48, f"unexpected cave size: {len(c)}"
    return bytes(c)


def build_intercept_jmp():
    """6 bytes: JMP CAVE_CODE_VA + NOP"""
    jmp = b'\xE9' + rel32(INTERCEPT_VA, CAVE_CODE_VA) + b'\x90'
    assert len(jmp) == 6
    return jmp


def build_cave_b_code():
    """
    Cave_B at CAVE_B_VA (path B: alternate DXDraw vtable+0x58 flip).

    At entry: EAX = DXDraw object, ECX = DXDraw vtable, EBX = caller's EBX.
    Saves all regs, does the same Blt scale as cave_A (when state==1),
    restores regs, reproduces the 3 pushes that were overwritten, calls
    [ECX+0x58] (DXDraw Flip), then jumps to RETURN_B_VA (skipping the
    mangled tail bytes 51 58 at 0x414D7A-7B).
    """
    c  = bytearray()
    va = CAVE_B_VA

    # CMP DWORD [GAME_STATE_VA], 1    83 3D xx xx xx xx 01    7 bytes
    c += b'\x83\x3D' + u32(GAME_STATE_VA) + b'\x01'; va += 7

    # JNE .do_flip                    75 ??                   2 bytes
    jne_off = len(c)
    c += b'\x75\x00'; va += 2

    # CMP DWORD [INTER_BUF_VA], 0    83 3D xx xx xx xx 00    7 bytes
    c += b'\x83\x3D' + u32(INTER_BUF_VA) + b'\x00'; va += 7

    # JE  .do_flip                    74 ??                   2 bytes
    je_off = len(c)
    c += b'\x74\x00'; va += 2

    # PUSHAD                          60                      1 byte
    c += b'\x60'; va += 1

    # --- Blt 1: intermediate <- back buffer {0,0,640,480} ---
    c += b'\xA1' + u32(INTER_BUF_VA); va += 5   # MOV EAX, [INTER_BUF_VA]
    c += b'\x8B\x08'; va += 2                    # MOV ECX, [EAX]  (vtable)
    c += b'\x6A\x00'; va += 2                    # PUSH 0  (lpDDBltFx)
    c += b'\x6A\x00'; va += 2                    # PUSH 0  (dwFlags)
    c += b'\x68' + u32(SRCRECT_VA); va += 5      # PUSH SRCRECT_VA
    c += b'\x8B\x15' + u32(BACK_BUF_VA); va += 6 # MOV EDX, [BACK_BUF_VA]
    c += b'\x52'; va += 1                         # PUSH EDX  (lpDDSrcSurface)
    c += b'\x6A\x00'; va += 2                    # PUSH 0  (lpDestRect = NULL)
    c += b'\x50'; va += 1                         # PUSH EAX  (this = intermediate)
    c += b'\xFF\x51\x14'; va += 3                # CALL [ECX+0x14]  Blt

    # --- Blt 2: back buffer {320,0,2240,1440} <- intermediate ---
    c += b'\xA1' + u32(BACK_BUF_VA); va += 5    # MOV EAX, [BACK_BUF_VA]
    c += b'\x8B\x08'; va += 2                    # MOV ECX, [EAX]  (vtable)
    c += b'\x6A\x00'; va += 2                    # PUSH 0  (lpDDBltFx)
    c += b'\x6A\x00'; va += 2                    # PUSH 0  (dwFlags)
    c += b'\x68' + u32(SRCRECT_VA); va += 5      # PUSH SRCRECT_VA
    c += b'\x8B\x15' + u32(INTER_BUF_VA); va += 6 # MOV EDX, [INTER_BUF_VA]
    c += b'\x52'; va += 1                         # PUSH EDX  (lpDDSrcSurface)
    c += b'\x68' + u32(DESTRECT_VA); va += 5     # PUSH DESTRECT_VA
    c += b'\x50'; va += 1                         # PUSH EAX  (this = back buffer)
    c += b'\xFF\x51\x14'; va += 3                # CALL [ECX+0x14]  Blt

    # POPAD                           61                      1 byte
    c += b'\x61'; va += 1

    # .do_flip — fix JNE and JE rel8
    do_flip_off = len(c)
    c[jne_off + 1] = do_flip_off - (jne_off + 2)
    c[je_off  + 1] = do_flip_off - (je_off  + 2)

    # Reproduce overwritten path-B pushes + call
    # PUSH 0                          6A 00                   2 bytes
    c += b'\x6A\x00'; va += 2
    # PUSH EBX                        53                      1 byte  (EBX restored by POPAD)
    c += b'\x53'; va += 1
    # PUSH EAX                        50                      1 byte  (EAX = DXDraw obj, restored by POPAD)
    c += b'\x50'; va += 1
    # CALL [ECX+0x58]                 FF 51 58                3 bytes (ECX = DXDraw vtable, restored by POPAD)
    c += b'\xFF\x51\x58'; va += 3
    # JMP RETURN_B_VA                 E9 xx xx xx xx          5 bytes  (skip mangled 51 58 at 0x414D7A-7B)
    c += b'\xE9' + rel32(va, RETURN_B_VA); va += 5

    assert va == CAVE_B_VA + len(c), f"cave_B VA tracking mismatch: {va:08X} vs {CAVE_B_VA+len(c):08X}"
    return bytes(c)


def build_intercept_b_jmp():
    """5 bytes: JMP CAVE_B_VA  (replaces 6A 00 53 50 FF at 0x414D75)"""
    jmp = b'\xE9' + rel32(INTERCEPT_B_VA, CAVE_B_VA)
    assert len(jmp) == 5
    return jmp


ORIG_B_BYTES   = bytes([0x6A, 0x00, 0x53, 0x50, 0xFF])    # 5 bytes before CALL [ECX+0x58]
ORIG_FLIP_CD   = bytes([0xFF, 0x15, 0x04, 0xD5, 0x45, 0x00])  # same 6-byte Flip CALL at sites C and D


def build_cx_cave_code(cave_va, return_va):
    """
    Generic 83-byte pre-Flip scale cave for sites where the CALL [Flip IAT] is
    directly preceded by a PUSH of a flag (the only Flip argument).

    The flag is already on the stack when we enter this cave (caller pushed it
    before the CALL we replaced with JMP). We do Blts (if state==1), then
    CALL [Flip IAT] — the flag sits at [ESP+4] relative to the CALL's frame,
    exactly where Flip expects arg1.  After the CALL we JMP to return_va (the
    instruction that follows the original CALL in the original code).

    No PUSH EBX adjustment needed here — path A's cave_A had a stray PUSH EBX
    from 0x414D09 that was handled by ADD ESP,4 at RETURN_VA. Sites C and D
    have no such extra push; return_va handles its own stack.
    """
    c  = bytearray()
    va = cave_va

    # CMP DWORD [GAME_STATE_VA], 1    83 3D xx xx xx xx 01    7 bytes
    # menu state is 1 (confirmed: all existing centering caves use state==1)
    c += b'\x83\x3D' + u32(GAME_STATE_VA) + b'\x01'; va += 7

    # JNE .do_flip                    75 ??                   2 bytes
    jne_off = len(c)
    c += b'\x75\x00'; va += 2

    # CMP DWORD [INTER_BUF_VA], 0    83 3D xx xx xx xx 00    7 bytes
    # Null-guard: skip Blts if intermediate surface not yet initialised
    c += b'\x83\x3D' + u32(INTER_BUF_VA) + b'\x00'; va += 7

    # JE  .do_flip                    74 ??                   2 bytes
    je_off = len(c)
    c += b'\x74\x00'; va += 2

    # PUSHAD                          60                      1 byte
    c += b'\x60'; va += 1

    # --- Blt 1: intermediate <- back buffer {0,0,640,480} ---
    c += b'\xA1' + u32(INTER_BUF_VA); va += 5
    c += b'\x8B\x08'; va += 2
    c += b'\x6A\x00'; va += 2  # lpDDBltFx
    c += b'\x6A\x00'; va += 2  # dwFlags = 0
    c += b'\x68' + u32(SRCRECT_VA); va += 5
    c += b'\x8B\x15' + u32(BACK_BUF_VA); va += 6
    c += b'\x52'; va += 1      # lpDDSrcSurface = backbuf
    c += b'\x6A\x00'; va += 2  # lpDestRect = NULL
    c += b'\x50'; va += 1      # this = intermediate
    c += b'\xFF\x51\x14'; va += 3

    # --- Blt 2: back buffer {0,0,1920,1440} <- intermediate ---
    c += b'\xA1' + u32(BACK_BUF_VA); va += 5
    c += b'\x8B\x08'; va += 2
    c += b'\x6A\x00'; va += 2  # lpDDBltFx
    c += b'\x6A\x00'; va += 2  # dwFlags = 0
    c += b'\x68' + u32(SRCRECT_VA); va += 5
    c += b'\x8B\x15' + u32(INTER_BUF_VA); va += 6
    c += b'\x52'; va += 1      # lpDDSrcSurface = intermediate
    c += b'\x68' + u32(DESTRECT_VA); va += 5
    c += b'\x50'; va += 1      # this = back buffer
    c += b'\xFF\x51\x14'; va += 3

    # POPAD                           61                      1 byte
    c += b'\x61'; va += 1

    # .do_flip — fix JNE and JE rel8
    do_flip_off = len(c)
    c[jne_off + 1] = do_flip_off - (jne_off + 2)
    c[je_off  + 1] = do_flip_off - (je_off  + 2)

    # CALL [FLIP_IAT_VA]              FF 15 xx xx xx xx       6 bytes
    c += b'\xFF\x15' + u32(FLIP_IAT_VA); va += 6
    # JMP return_va                   E9 xx xx xx xx          5 bytes
    c += b'\xE9' + rel32(va, return_va); va += 5

    assert va == cave_va + len(c), f"cx cave VA mismatch: {va:08X} vs {cave_va+len(c):08X}"
    assert len(c) == 92, f"cx cave size: {len(c)} != 92"
    return bytes(c)


def build_cx_intercept(intercept_va, cave_va):
    """6 bytes: JMP cave_va + NOP  (replaces 6-byte CALL [Flip IAT])"""
    jmp = b'\xE9' + rel32(intercept_va, cave_va) + b'\x90'
    assert len(jmp) == 6
    return jmp


def apply():
    srcrect, destrect, fullrect = build_rects()
    cave_code = build_cave_code()
    jmp = build_intercept_jmp()

    print(f"srcRect  : {struct.unpack('<4i', srcrect)}")
    print(f"destRect : {struct.unpack('<4i', destrect)}")
    print(f"Cave code: {len(cave_code)} bytes @ VA 0x{CAVE_CODE_VA:08X}")
    print(f"  JNE rel8 = 0x{cave_code[8]:02X} ({cave_code[8]})")
    print(f"Intercept: {jmp.hex(' ')} @ VA 0x{INTERCEPT_VA:08X}")

    with open(EXE, 'rb') as f:
        exe = bytearray(f.read())

    # --- disable COLORFILL centering (JNE -> JMP at 0x45C1F5+7) ---
    # COLORFILL cave checks state==3 (menu) and adds +960/+480 to RECT fields.
    # Since we scale everything post-render, we need COLORFILL at original coords.
    # Change JNE (75) to JMP (EB) so the ADD offsets are always skipped.
    COLORFILL_JNE_VA = 0x45C1F5 + 7
    cf_byte = exe[fo(COLORFILL_JNE_VA)]
    print(f"\nCOLORFILL JNE @ 0x{COLORFILL_JNE_VA:08X}: {hex(cf_byte)}", end='')
    if cf_byte == 0x75:
        exe[fo(COLORFILL_JNE_VA)] = 0xEB
        print(" -> EB (JMP, centering disabled)")
    elif cf_byte == 0xEB:
        print(" [already JMP]")
    else:
        print(f" WARNING: unexpected {hex(cf_byte)}")

    # --- zero BltFast centering values ---
    bf_x = bytes(exe[fo(BLTFAST_ADD_X_VA):fo(BLTFAST_ADD_X_VA)+4])
    bf_y = bytes(exe[fo(BLTFAST_ADD_Y_VA):fo(BLTFAST_ADD_Y_VA)+4])
    print(f"\nBltFast ADD X imm @ 0x{BLTFAST_ADD_X_VA:08X}: {bf_x.hex(' ')}", end='')
    if bf_x == b'\xC0\x03\x00\x00':
        exe[fo(BLTFAST_ADD_X_VA):fo(BLTFAST_ADD_X_VA)+4] = b'\x00\x00\x00\x00'
        print(" -> zeroed")
    elif bf_x == b'\x00\x00\x00\x00':
        print(" [already zero]")
    else:
        print(f" WARNING: unexpected value")

    print(f"BltFast ADD Y imm @ 0x{BLTFAST_ADD_Y_VA:08X}: {bf_y.hex(' ')}", end='')
    if bf_y == b'\xE0\x01\x00\x00':
        exe[fo(BLTFAST_ADD_Y_VA):fo(BLTFAST_ADD_Y_VA)+4] = b'\x00\x00\x00\x00'
        print(" -> zeroed")
    elif bf_y == b'\x00\x00\x00\x00':
        print(" [already zero]")
    else:
        print(f" WARNING: unexpected value")

    # --- zero Blt bg destRect ---
    bg_rect = bytes(exe[fo(BLT_BG_DESTRECT):fo(BLT_BG_DESTRECT)+16])
    want_zeroed = struct.pack('<4i', 0, 0, 640, 480)
    want_orig   = struct.pack('<4i', 960, 480, 1600, 960)
    print(f"\nBlt bg destRect @ 0x{BLT_BG_DESTRECT:08X}: {bg_rect.hex(' ')}", end='')
    if bg_rect == want_orig:
        exe[fo(BLT_BG_DESTRECT):fo(BLT_BG_DESTRECT)+16] = want_zeroed
        print(" -> {0,0,640,480}")
    elif bg_rect == want_zeroed:
        print(" [already {0,0,640,480}]")
    else:
        print(" WARNING: unexpected value — overwriting anyway")
        exe[fo(BLT_BG_DESTRECT):fo(BLT_BG_DESTRECT)+16] = want_zeroed

    # --- zero car D3D190 cave ADD immediate ---
    car_imm = bytes(exe[fo(CAR_ADD_IMME_VA):fo(CAR_ADD_IMME_VA)+4])
    print(f"\nCar ADD imm @ 0x{CAR_ADD_IMME_VA:08X}: {car_imm.hex(' ')}", end='')
    if car_imm == b'\x80\x87\x25\x00':
        exe[fo(CAR_ADD_IMME_VA):fo(CAR_ADD_IMME_VA)+4] = b'\x00\x00\x00\x00'
        print(" -> zeroed")
    elif car_imm == b'\x00\x00\x00\x00':
        print(" [already zero]")
    else:
        print(f" WARNING: unexpected value")

    # --- write RECT data ---
    exe[fo(SRCRECT_VA) :fo(SRCRECT_VA)  + 16] = srcrect
    exe[fo(DESTRECT_VA):fo(DESTRECT_VA) + 16] = destrect
    print(f"\nWrote srcRect  @ VA 0x{SRCRECT_VA:08X}  (file 0x{fo(SRCRECT_VA):05X}): {srcrect.hex(' ')}")
    print(f"Wrote destRect @ VA 0x{DESTRECT_VA:08X}  (file 0x{fo(DESTRECT_VA):05X}): {destrect.hex(' ')}")

    leftBarRect  = struct.pack('<4i', 0, 0, 320, 1440)
    blackRowRect = struct.pack('<4i', 0, 500, 320, 501)
    exe[fo(LEFT_BAR_RECT_VA) :fo(LEFT_BAR_RECT_VA)  + 16] = leftBarRect
    exe[fo(BLACK_ROW_RECT_VA):fo(BLACK_ROW_RECT_VA) + 16] = blackRowRect
    print(f"Wrote leftBar  @ VA 0x{LEFT_BAR_RECT_VA:08X}  (file 0x{fo(LEFT_BAR_RECT_VA):05X}): {{0,0,320,1440}}")
    print(f"Wrote blackRow @ VA 0x{BLACK_ROW_RECT_VA:08X}  (file 0x{fo(BLACK_ROW_RECT_VA):05X}): {{0,500,320,501}}")

    # --- write cave code ---
    exe[fo(CAVE_CODE_VA):fo(CAVE_CODE_VA) + len(cave_code)] = cave_code
    print(f"Wrote cave code ({len(cave_code)}B) @ VA 0x{CAVE_CODE_VA:08X}")

    # --- verify / write intercept JMP (path A) ---
    site = bytes(exe[fo(INTERCEPT_VA):fo(INTERCEPT_VA)+6])
    if site == jmp:
        print(f"Intercept A JMP already present at VA 0x{INTERCEPT_VA:08X} — cave rewritten only.")
    elif site == ORIG_FLIP_CALL:
        exe[fo(INTERCEPT_VA):fo(INTERCEPT_VA)+6] = jmp
        print(f"Wrote intercept A JMP @ VA 0x{INTERCEPT_VA:08X} (file 0x{fo(INTERCEPT_VA):05X}): {jmp.hex(' ')}")
    else:
        print(f"WARNING: intercept A site = {site.hex(' ')} — expected {ORIG_FLIP_CALL.hex(' ')}")
        print("  Overwriting with JMP anyway.")
        exe[fo(INTERCEPT_VA):fo(INTERCEPT_VA)+6] = jmp

    # --- write cave_B and intercept (path B) ---
    cave_b = build_cave_b_code()
    jmp_b  = build_intercept_b_jmp()
    print(f"\nCave B code: {len(cave_b)} bytes @ VA 0x{CAVE_B_VA:08X}")

    exe[fo(CAVE_B_VA):fo(CAVE_B_VA) + len(cave_b)] = cave_b

    site_b = bytes(exe[fo(INTERCEPT_B_VA):fo(INTERCEPT_B_VA)+5])
    if site_b == jmp_b:
        print(f"Intercept B JMP already present at VA 0x{INTERCEPT_B_VA:08X} — cave_B rewritten only.")
    elif site_b == ORIG_B_BYTES:
        exe[fo(INTERCEPT_B_VA):fo(INTERCEPT_B_VA)+5] = jmp_b
        # NOP the 2 destroyed tail bytes of CALL [ECX+0x58] at 0x414D7A-7B
        exe[fo(INTERCEPT_B_VA)+5] = 0x90
        exe[fo(INTERCEPT_B_VA)+6] = 0x90
        print(f"Wrote intercept B JMP @ VA 0x{INTERCEPT_B_VA:08X}: {jmp_b.hex(' ')}")
        print(f"  NOP'd tail bytes at VA 0x{INTERCEPT_B_VA+5:08X}-{INTERCEPT_B_VA+6:08X}")
    else:
        expected_jmp_b = jmp_b
        print(f"WARNING: intercept B site = {site_b.hex(' ')} — expected {ORIG_B_BYTES.hex(' ')}")
        print("  Overwriting with JMP + NOPs anyway.")
        exe[fo(INTERCEPT_B_VA):fo(INTERCEPT_B_VA)+5] = jmp_b
        exe[fo(INTERCEPT_B_VA)+5] = 0x90
        exe[fo(INTERCEPT_B_VA)+6] = 0x90

    # --- write cave_C and cave_D (the actual menu/outer-loop Flip sites) ---
    cave_c = build_cx_cave_code(CAVE_C_VA, RETURN_C_VA)
    cave_d = build_cx_cave_code(CAVE_D_VA, RETURN_D_VA)
    jmp_c  = build_cx_intercept(INTERCEPT_C_VA, CAVE_C_VA)
    jmp_d  = build_cx_intercept(INTERCEPT_D_VA, CAVE_D_VA)
    print(f"\nCave C code: {len(cave_c)} bytes @ VA 0x{CAVE_C_VA:08X}  (intercept 0x{INTERCEPT_C_VA:08X})")
    print(f"Cave D code: {len(cave_d)} bytes @ VA 0x{CAVE_D_VA:08X}  (intercept 0x{INTERCEPT_D_VA:08X})")

    exe[fo(CAVE_C_VA):fo(CAVE_C_VA) + len(cave_c)] = cave_c
    exe[fo(CAVE_D_VA):fo(CAVE_D_VA) + len(cave_d)] = cave_d

    for label, int_va, jmp_x in [("C", INTERCEPT_C_VA, jmp_c), ("D", INTERCEPT_D_VA, jmp_d)]:
        site_x = bytes(exe[fo(int_va):fo(int_va)+6])
        if site_x == jmp_x:
            print(f"Intercept {label} JMP already present at VA 0x{int_va:08X}")
        elif site_x == ORIG_FLIP_CD:
            exe[fo(int_va):fo(int_va)+6] = jmp_x
            print(f"Wrote intercept {label} JMP @ VA 0x{int_va:08X}: {jmp_x.hex(' ')}")
        else:
            print(f"WARNING: intercept {label} site = {site_x.hex(' ')} — overwriting anyway")
            exe[fo(int_va):fo(int_va)+6] = jmp_x

    with open(EXE, 'wb') as f:
        f.write(exe)
    print("\nDone.")


def verify():
    srcrect, destrect, fullrect = build_rects()
    cave_code = build_cave_code()
    jmp = build_intercept_jmp()

    with open(EXE, 'rb') as f:
        exe = f.read()

    def check(label, va, expected, actual=None):
        if actual is None:
            actual = exe[fo(va):fo(va)+len(expected)]
        ok = "OK" if actual == expected else "MISMATCH"
        print(f"  {label:40s} {bytes(actual).hex(' ')}  [{ok}]")
        if actual != expected:
            print(f"    expected: {expected.hex(' ')}")

    print("--- centering zeroes ---")
    COLORFILL_JNE_VA = 0x45C1F5 + 7
    cf_byte = exe[fo(COLORFILL_JNE_VA)]
    cf_ok = "OK (JMP=disabled)" if cf_byte == 0xEB else ("JNE=active" if cf_byte == 0x75 else "UNEXPECTED")
    print(f"  {'COLORFILL JNE->JMP (expect EB)':40s} {hex(cf_byte)}  [{cf_ok}]")
    check("BltFast ADD X imm (expect 00 00 00 00)", BLTFAST_ADD_X_VA,
          b'\x00\x00\x00\x00')
    check("BltFast ADD Y imm (expect 00 00 00 00)", BLTFAST_ADD_Y_VA,
          b'\x00\x00\x00\x00')
    check("Blt bg destRect   (expect {0,0,640,480})", BLT_BG_DESTRECT,
          struct.pack('<4i', 0, 0, 640, 480))
    check("Car ADD imm       (expect 00 00 00 00)", CAR_ADD_IMME_VA,
          b'\x00\x00\x00\x00')

    print("--- scale data ---")
    check(f"srcRect  VA 0x{SRCRECT_VA:08X}", SRCRECT_VA, srcrect)
    check(f"destRect VA 0x{DESTRECT_VA:08X}", DESTRECT_VA, destrect)
    check(f"fullRect VA 0x{FULLRECT_VA:08X}", FULLRECT_VA, fullrect)

    print("--- cave_A code ---")
    actual_cave = exe[fo(CAVE_CODE_VA):fo(CAVE_CODE_VA)+len(cave_code)]
    ok = "OK" if actual_cave == cave_code else "MISMATCH"
    print(f"  Cave code ({len(cave_code)}B) @ VA 0x{CAVE_CODE_VA:08X}: [{ok}]")
    if actual_cave != cave_code:
        print(f"    actual:   {bytes(actual_cave).hex(' ')}")
        print(f"    expected: {cave_code.hex(' ')}")

    print("--- intercept A ---")
    site = exe[fo(INTERCEPT_VA):fo(INTERCEPT_VA)+6]
    s_ok = "JMP (patched)" if site == jmp else \
           ("ORIG CALL (unpatched)" if site == ORIG_FLIP_CALL else "MISMATCH")
    print(f"  Intercept A VA 0x{INTERCEPT_VA:08X}: {bytes(site).hex(' ')}  [{s_ok}]")

    print("--- cave_B + intercept B ---")
    cave_b = build_cave_b_code()
    jmp_b  = build_intercept_b_jmp()
    actual_b = exe[fo(CAVE_B_VA):fo(CAVE_B_VA)+len(cave_b)]
    ok_b = "OK" if actual_b == cave_b else "MISMATCH"
    print(f"  Cave_B ({len(cave_b)}B) @ VA 0x{CAVE_B_VA:08X}: [{ok_b}]")
    if actual_b != cave_b:
        print(f"    actual:   {bytes(actual_b).hex(' ')}")
        print(f"    expected: {cave_b.hex(' ')}")
    site_b = exe[fo(INTERCEPT_B_VA):fo(INTERCEPT_B_VA)+7]
    expected_b = jmp_b + bytes([0x90, 0x90])
    sb_ok = "JMP+NOPs (patched)" if bytes(site_b) == expected_b else \
            ("ORIG BYTES (unpatched)" if bytes(site_b[:5]) == ORIG_B_BYTES else "MISMATCH")
    print(f"  Intercept B VA 0x{INTERCEPT_B_VA:08X}: {bytes(site_b).hex(' ')}  [{sb_ok}]")


if __name__ == "__main__":
    import sys
    if "--verify" in sys.argv:
        verify()
    else:
        apply()
