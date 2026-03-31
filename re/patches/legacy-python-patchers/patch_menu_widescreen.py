"""
patch_menu_widescreen.py
========================
Canonical single-file widescreen fix for the TD5_d3d.exe main menu,
loading screens, legal/splash screens, and FPS debug screen.
Target: 2560 × 1440.  Change W / H below for other resolutions.

Usage
-----
  python patch_menu_widescreen.py           # apply all patches
  python patch_menu_widescreen.py --verify  # check current state

Depends on
----------
  td5_widescreen.py must be run first (sets WinMain / menu-init resolution
  globals and M2DX.DLL default mode).  This script handles only the rendering
  centering and pre-Flip scaling.

Approach
--------
Let all menu drawing happen at internal 640 × 480 coordinates, then
intercept every Flip call site and stretch the 640 × 480 back-buffer region
to the full target resolution via a two-blit intermediate-surface pass.

Why two blits, not a same-surface self-Blt?
  Blitting {0,0,640,480} → {0,0,W,H} on the same DirectDraw surface is
  undefined when the regions overlap.  dgVoodoo2 and real hardware may read
  from the destination during the write pass.  Using the secondary 640 × 480
  work surface (DAT_00496264) as an intermediate avoids the overlap entirely.

Menu rendering pipeline (dgVoodoo2 hardware mode, DAT_004951ec == 1)
---------------------------------------------------------------------
  Game draws all UI into offscreen work surfaces:
    DAT_00496260  primary work surface  (640 × 480)
    DAT_00496264  secondary work surface (640 × 480)

  "Present" helpers copy that content to the back-buffer (DAT_00495220):
    PresentPrimaryFrontendBufferViaCopy  0x424af0  via Copy16BitSurfaceRect flag=0x10
    PresentPrimaryFrontendBuffer         0x424ca0  BltFast flag=0x10 direct (coords ignored)
    PresentPrimaryFrontendRect           0x424d40  BltFast flag=0x10 direct (coords ignored)
    PresentSecondaryFrontendRect         0x424cf0  BltFast flag=0x10 direct (coords ignored)
    PresentSecondaryFrontendRectViaCopy  0x424c10  via Copy16BitSurfaceRect flag=0x10
    FlushFrontendSpriteBlits             0x425540  Copy16BitSurfaceRect flag=0x11 per sprite

  dgVoodoo2 ignores destX/destY for BltFast flag=0x10 → content lands at
  (0,0) in the back-buffer regardless of the coordinate arguments.
  For flag=0x11 the coordinates ARE honoured (hence sprite-flag patch below).

  All content therefore sits in the {0,0,640,480} region of the back-buffer.

  RunFrontendDisplayLoop (0x414b50) calls Flip on one of two paths:
    Path A (DAT_0049523c != 0): CALL [DXDraw::Flip IAT] at 0x414D0A
    Path B (DAT_0049523c == 0): vtable+0x58 call at 0x414D75

  Before every Flip our cave:
    1. Blt back_buf{0,0,640,480} → intermediate  (DAT_00496264, no overlap)
    2. Blt intermediate          → back_buf{0,0,W,H}  (stretch to full res)
    3. Call original Flip

  The same cave is also installed at two additional Flip call sites in the
  outer render/software-render loops:
    Path C: CALL [DXDraw::Flip IAT] at 0x42CBBF  (DisplayLoadingScreenImage)
    Path D: CALL [DXDraw::Flip IAT] at 0x442502  (RunMainGameLoop state 3)

State gate
----------
  The scale caves check game_state != 2 (race) and intermediate surface != NULL.
  This activates the stretch for states 0 (legal screens), 1 (menu), and
  3 (FPS debug), while skipping race frames where D3D handles full-res
  rendering natively.

  Race rendering is already fully widescreen-correct:
    - D3D viewport: CreateAndBindViewport(width, height) from M2DX.DLL
    - Race HUD: D3D textured quads, InitializeRaceHudLayout scales dynamically
    - Presentation: DXD3D::EndScene calls DXDraw::Flip internally

Supplementary patches
---------------------
  Surface pre-allocation (cave_E at 0x45C2B0):
    Intercepts ShowLegalScreens call at 0x4421E8 to create the intermediate
    surface (DAT_00496264) before state 0 legal screens run, so the scale
    caves in paths C/D have an intermediate surface available during state 0.

  Force-Flip presentation (0x424F0C):
    NOPs the JNZ that would skip forcing DAT_0049523c=1 in fullscreen mode.
    This ensures the Flip presentation path (not the software scanline copy)
    is always active, so the scale caves always get a chance to run.

Patches applied
---------------
  Zeroing offsets (centering approach superseded by scale approach):
    BltFast cave ADD X imm  0x45C235  960 → 0
    BltFast cave ADD Y imm  0x45C23C  480 → 0
    d3d190 cave ADD EAX imm 0x45C303  0x258780 → 0  (car write-pointer shift)
    Blt bg destRect         0x45C250  {960,480,1600,960} → {0,0,640,480}

  COLORFILL centering disable:
    JNE → JMP at 0x45C1FC  (cave 0x45C1F5; stops centering ADD on menu fills)

  Sprite loop flag (FlushFrontendSpriteBlits, VA 0x42557c):
    PUSH 0x10 → PUSH 0x11  (add DDBLTFAST_SRCCOLORKEY)
    Reason: dgVoodoo2 ignores destX/destY for flag=0x10 calls; the
    background-restore loop in FlushFrontendSpriteBlits saves and restores
    sprite regions by saved coordinate – those coordinates must be honoured
    so the restore goes to the right place.  The composite primary surface
    has no colour key, so SRCCOLORKEY is benign.

  Scale rect data:
    srcRect  @ 0x45C310  {0, 0, 640, 480}
    destRect @ 0x45C320  {0, 0, W, H}

  Pre-Flip scale caves (two-blit via intermediate):
    cave_A  0x45C330  92B  intercepts path A (IAT Flip, menu)
    cave_B  0x45C38C  93B  intercepts path B (vtable Flip, menu software path)
    cave_C  0x45C3E9  92B  intercepts path C (IAT Flip, DisplayLoadingScreenImage)
    cave_D  0x45C445  92B  intercepts path D (IAT Flip, RunMainGameLoop state 3)

  Surface pre-allocation:
    cave_E  0x45C2B0  33B  intercepts ShowLegalScreens call at 0x4421E8

  Force-Flip:
    NOP at 0x424F0C  (2B: 75 08 → 90 90)

Code cave layout in .rdata zero-pad (0x45C000 – 0x45D000)
----------------------------------------------------------
  0x45C1A1  62B   dead (old centering cave, superseded)
  0x45C1F5  52B   COLORFILL cave  (JNE at +7 flipped to JMP to disable adds)
  0x45C229  34B   BltFast trampoline  (ADD offsets zeroed; JNE NOP'd)
  0x45C250  16B   Blt bg destRect     {0,0,640,480}
  0x45C260  16B   Blt bg srcRect      {0,0,640,480}  (used by cave 0x45C270)
  0x45C270  56B   menu-bg alternate Blt cave  (intercept at 0x424B21)
  0x45C2B0  33B   cave_E  (surface pre-alloc before ShowLegalScreens)
  0x45C2D3  35B   dead (old car-bg cave 2, superseded by d3d190 cave)
  0x45C2F6  25B   d3d190 cave  (ADD EAX zeroed; JMP from 0x40D369)
  0x45C310  16B   srcRect  data
  0x45C320  16B   destRect data
  0x45C330  92B   cave_A code
  0x45C38C  93B   cave_B code
  0x45C3E9  92B   cave_C code
  0x45C445  92B   cave_D code
"""

import struct

EXE        = "D:/Descargas/Test Drive 5 ISO/TD5_d3d.exe"
IMAGE_BASE = 0x400000

# ── Target resolution ─────────────────────────────────────────────────────────
W, H           = 2560, 1440
GAME_W, GAME_H = 640, 480

# ── Key globals ───────────────────────────────────────────────────────────────
GAME_STATE_VA = 0x4C3CE8   # int: 0=intro/movie, 1=menu, 2=race, 3=FPS-replay
BACK_BUF_VA   = 0x495220   # IDirectDrawSurface*  back-buffer
INTER_BUF_VA  = 0x496264   # IDirectDrawSurface*  secondary work surface (640×480)
#                            Used by caves A/B (valid during menu, freed on race init)
CAVE_INTER_VA = 0x4C4F00   # IDirectDrawSurface*  our dedicated 640×480 intermediate
#                            Pure BSS (VA > file EOF), never written by game code.
#                            Lazy-allocated by cave C on first race start.
#                            Used by caves C/D (loading screen + FPS replay).
FLIP_IAT_VA   = 0x45D504   # IAT slot: DXDraw::Flip

# ── Flip intercept sites ──────────────────────────────────────────────────────
INTERCEPT_A_VA = 0x414D0A   # CALL [FLIP_IAT_VA]  6B  (path A, IAT)
RETURN_A_VA    = 0x414D10
INTERCEPT_B_VA = 0x414D75   # 6A 00 53 50 FF       5B  (path B, vtable+0x58)
RETURN_B_VA    = 0x414D7C   # skip 2 NOP'd tail bytes
INTERCEPT_C_VA = 0x42CBBF   # CALL [FLIP_IAT_VA]  6B
RETURN_C_VA    = 0x42CBC5
INTERCEPT_D_VA = 0x442502   # CALL [FLIP_IAT_VA]  6B
RETURN_D_VA    = 0x442508

# ── Cave addresses ────────────────────────────────────────────────────────────
SRCRECT_VA  = 0x45C310   # 16B: {0, 0, 640, 480}
DESTRECT_VA = 0x45C320   # 16B: {0, 0, W, H}
CAVE_A_VA     = 0x45C330   # 92B  (uses INTER_BUF_VA, menu path)
CAVE_B_VA     = 0x45C38C   # 93B  (uses INTER_BUF_VA, menu path)
CAVE_C_VA     = 0x45C3E9   # 92B  (dead slot, kept for layout continuity)
CAVE_D_VA     = 0x45C445   # 92B  (rebuilt with CAVE_INTER_VA, FPS replay)
CAVE_C_NEW_VA = 0x45C500   # 119B (lazy-alloc cave for loading screen; after cave D)

# ── Zeroing targets ───────────────────────────────────────────────────────────
BLTFAST_ADD_X_VA = 0x45C235   # imm32 inside BltFast cave: ADD [EBP+8], X
BLTFAST_ADD_Y_VA = 0x45C23C   # imm32 inside BltFast cave: ADD [EBP+C], Y
BLT_BG_DESTRECT  = 0x45C250   # 16B destRect for alternate bg Blt
D3D190_ADD_VA    = 0x45C303   # imm32 inside d3d190 cave: ADD EAX, offset

# ── Surface pre-allocation cave (state 0 legal screens) ─────────────────────
CREATE_SURFACE_VA = 0x411F00   # CreateTrackedFrontendSurface(height, width) → EAX
SHOW_LEGAL_CALL_VA = 0x4421E8  # CALL ShowLegalScreens in RunMainGameLoop case 0
SHOW_LEGAL_RETURN_VA = 0x4421ED
SHOW_LEGAL_TARGET_VA = 0x42C8E0  # ShowLegalScreens entry
CAVE_E_VA   = 0x45C2B0   # 33B  (reuses dead car-bg cave 1)

# ── Force-Flip patch ────────────────────────────────────────────────────────
FORCE_FLIP_VA = 0x424F0C   # JNZ +8 (75 08) → NOP NOP (90 90)

# ── Misc patches ─────────────────────────────────────────────────────────────
COLORFILL_JNE_VA = 0x45C1FC   # byte: JNE(75) → JMP(EB)  disables centering adds
SPRITE_FLAG_VA   = 0x42557C   # operand of PUSH: 0x10 → 0x11

# ── Helpers ───────────────────────────────────────────────────────────────────
fo    = lambda va: va - IMAGE_BASE
u32   = lambda x:  struct.pack('<I', x)
rel32 = lambda fr, to: struct.pack('<i', to - (fr + 5))

ORIG_FLIP_IAT = bytes([0xFF, 0x15, 0x04, 0xD5, 0x45, 0x00])   # CALL [Flip IAT]
ORIG_B_BYTES  = bytes([0x6A, 0x00, 0x53, 0x50, 0xFF])          # pre-CALL pushes


# ── Cave builders ─────────────────────────────────────────────────────────────

def _blt_inter_from_backbuf(c_list, va_ref, inter_va):
    """Append 29-byte Blt block: intermediate ← back_buf {0,0,GAME_W,GAME_H}."""
    c_list += b'\xA1' + u32(inter_va)           # MOV EAX,[inter]        5
    c_list += b'\x8B\x08'                        # MOV ECX,[EAX] vtable   2
    c_list += b'\x6A\x00'                        # PUSH 0  lpDDBltFx      2
    c_list += b'\x6A\x00'                        # PUSH 0  dwFlags        2
    c_list += b'\x68' + u32(SRCRECT_VA)          # PUSH lpSrcRect         5
    c_list += b'\x8B\x15' + u32(BACK_BUF_VA)    # MOV EDX,[backbuf]      6
    c_list += b'\x52'                            # PUSH EDX lpSrcSurf     1
    c_list += b'\x6A\x00'                        # PUSH 0  lpDestRect     2
    c_list += b'\x50'                            # PUSH EAX this=inter    1
    c_list += b'\xFF\x51\x14'                    # CALL [vtable+0x14]     3
    va_ref[0] += 29


def _blt_backbuf_from_inter(c_list, va_ref, inter_va):
    """Append 32-byte Blt block: back_buf{0,0,W,H} ← intermediate."""
    c_list += b'\xA1' + u32(BACK_BUF_VA)        # MOV EAX,[backbuf]      5
    c_list += b'\x8B\x08'                        # MOV ECX,[EAX] vtable   2
    c_list += b'\x6A\x00'                        # PUSH 0  lpDDBltFx      2
    c_list += b'\x6A\x00'                        # PUSH 0  dwFlags        2
    c_list += b'\x68' + u32(SRCRECT_VA)          # PUSH lpSrcRect         5
    c_list += b'\x8B\x15' + u32(inter_va)        # MOV EDX,[inter]        6
    c_list += b'\x52'                            # PUSH EDX lpSrcSurf     1
    c_list += b'\x68' + u32(DESTRECT_VA)         # PUSH lpDestRect        5
    c_list += b'\x50'                            # PUSH EAX this=backbuf  1
    c_list += b'\xFF\x51\x14'                    # CALL [vtable+0x14]     3
    va_ref[0] += 32


def build_scale_cave(cave_va, return_va, inter_va=None):
    """
    92-byte pre-Flip scale cave (paths A, D – IAT Flip).

    inter_va: which surface pointer global to use as intermediate.
      Caves A/B pass INTER_BUF_VA (game's secondary surface, valid during menu).
      Cave D passes CAVE_INTER_VA (our persistent surface, valid after first race).

    Entry: this cave was reached via a JMP that replaced CALL [FLIP_IAT_VA].
    If game_state!=2 and intermediate surface is allocated, performs two-blit
    stretch to full resolution before calling Flip and returning to caller.
    Skips only during active race (state 2) where D3D handles full-res natively.
    """
    if inter_va is None:
        inter_va = INTER_BUF_VA

    c   = bytearray()
    va  = [cave_va]   # mutable for the helper functions

    # CMP [game_state], 2  — skip scaling only during race
    c += b'\x83\x3D' + u32(GAME_STATE_VA) + b'\x02'; va[0] += 7
    jne_off = len(c)
    c += b'\x74\x00'; va[0] += 2                     # JE .do_flip

    # CMP [inter], 0
    c += b'\x83\x3D' + u32(inter_va) + b'\x00'; va[0] += 7
    je_off = len(c)
    c += b'\x74\x00'; va[0] += 2                     # JE  .do_flip

    c += b'\x60'; va[0] += 1                         # PUSHAD

    _blt_inter_from_backbuf(c, va, inter_va)
    _blt_backbuf_from_inter(c, va, inter_va)

    c += b'\x61'; va[0] += 1                         # POPAD

    # .do_flip — patch forward offsets
    do_flip = len(c)
    c[jne_off + 1] = do_flip - (jne_off + 2)
    c[je_off  + 1] = do_flip - (je_off  + 2)

    c += b'\xFF\x15' + u32(FLIP_IAT_VA); va[0] += 6  # CALL [Flip IAT]
    c += b'\xE9' + rel32(va[0], return_va); va[0] += 5  # JMP return

    assert len(c) == 92, f"cave size {len(c)} != 92"
    assert va[0] == cave_va + 92
    return bytes(c)


def build_scale_cave_c_lazy():
    """
    119-byte pre-Flip scale cave for path C (IAT Flip, DisplayLoadingScreenImage).

    Unlike caves A/B which use INTER_BUF_VA (the game's secondary work surface that
    gets freed during race init), this cave uses CAVE_INTER_VA — a dedicated BSS
    slot (0x4C4F00) that the game never touches.

    Lazy allocation: on first call (CAVE_INTER_VA == 0), calls
      CreateTrackedFrontendSurface(480, 640) → stores result in CAVE_INTER_VA.
    Safe because by the time DisplayLoadingScreenImage runs (race-init from menu),
    the frontend tracking table has been initialised and the DXDraw device is live.

    If allocation fails (EAX == 0 after CALL), falls through to do_flip without
    scaling — the loading screen appears at 640×480 top-left rather than crashing.
    """
    c  = bytearray()
    va = [CAVE_C_NEW_VA]

    # ── state gate ──────────────────────────────────────────────────────────
    # CMP [game_state], 2 → JE do_flip  (never true here, but keeps symmetry)
    c += b'\x83\x3D' + u32(GAME_STATE_VA) + b'\x02'; va[0] += 7
    je_state_off = len(c)
    c += b'\x74\x00'; va[0] += 2                 # JE .do_flip  (+0 patched)

    # ── lazy alloc ──────────────────────────────────────────────────────────
    # CMP [CAVE_INTER_VA], 0 → JNZ .has_surface  (already allocated?)
    c += b'\x83\x3D' + u32(CAVE_INTER_VA) + b'\x00'; va[0] += 7
    jnz_off = len(c)
    c += b'\x75\x00'; va[0] += 2                 # JNZ .has_surface (+0 patched)

    # PUSH GAME_H (480); PUSH GAME_W (640); CALL CreateTrackedFrontendSurface; ADD ESP,8
    c += b'\x68' + u32(GAME_H); va[0] += 5       # PUSH 480
    c += b'\x68' + u32(GAME_W); va[0] += 5       # PUSH 640
    c += b'\xE8' + rel32(va[0], CREATE_SURFACE_VA); va[0] += 5   # CALL
    c += b'\x83\xC4\x08'; va[0] += 3             # ADD ESP, 8

    # TEST EAX,EAX → JZ .do_flip  (allocation failed → skip scaling)
    c += b'\x85\xC0'; va[0] += 2                 # TEST EAX,EAX
    je_fail_off = len(c)
    c += b'\x74\x00'; va[0] += 2                 # JZ .do_flip   (+0 patched)

    # MOV [CAVE_INTER_VA], EAX  — store surface pointer
    c += b'\xA3' + u32(CAVE_INTER_VA); va[0] += 5

    # ── .has_surface ────────────────────────────────────────────────────────
    has_surface = len(c)
    c[jnz_off + 1] = has_surface - (jnz_off + 2)  # patch JNZ

    c += b'\x60'; va[0] += 1                     # PUSHAD

    _blt_inter_from_backbuf(c, va, CAVE_INTER_VA)
    _blt_backbuf_from_inter(c, va, CAVE_INTER_VA)

    c += b'\x61'; va[0] += 1                     # POPAD

    # ── .do_flip ────────────────────────────────────────────────────────────
    do_flip = len(c)
    c[je_state_off + 1] = do_flip - (je_state_off + 2)
    c[je_fail_off  + 1] = do_flip - (je_fail_off  + 2)

    c += b'\xFF\x15' + u32(FLIP_IAT_VA); va[0] += 6   # CALL [Flip IAT]
    c += b'\xE9' + rel32(va[0], RETURN_C_VA); va[0] += 5  # JMP return

    assert len(c) == 119, f"cave_C_lazy size {len(c)} != 119"
    assert va[0] == CAVE_C_NEW_VA + 119
    return bytes(c)


def build_cave_b():
    """
    93-byte pre-Flip scale cave for path B (vtable+0x58 Flip).

    Intercept replaces 5 bytes at 0x414D75 (PUSH 0; PUSH EBX; PUSH EAX
    immediately before CALL [ECX+0x58]).  The cave performs the same two-blit
    stretch then reproduces those pushes + the CALL [ECX+0x58], then jumps
    past the 2 NOP'd tail bytes to RETURN_B_VA.

    EAX and ECX are restored by POPAD (EAX = DXDraw obj, ECX = its vtable).
    """
    c   = bytearray()
    va  = [CAVE_B_VA]

    # CMP [game_state], 2  — skip scaling only during race
    c += b'\x83\x3D' + u32(GAME_STATE_VA) + b'\x02'; va[0] += 7
    jne_off = len(c)
    c += b'\x74\x00'; va[0] += 2                     # JE .do_flip

    c += b'\x83\x3D' + u32(INTER_BUF_VA) + b'\x00'; va[0] += 7
    je_off = len(c)
    c += b'\x74\x00'; va[0] += 2

    c += b'\x60'; va[0] += 1                         # PUSHAD

    _blt_inter_from_backbuf(c, va, INTER_BUF_VA)
    _blt_backbuf_from_inter(c, va, INTER_BUF_VA)

    c += b'\x61'; va[0] += 1                         # POPAD

    do_flip = len(c)
    c[jne_off + 1] = do_flip - (jne_off + 2)
    c[je_off  + 1] = do_flip - (je_off  + 2)

    # Reproduce the 5 overwritten bytes + original CALL [ECX+0x58]
    c += b'\x6A\x00'; va[0] += 2    # PUSH 0   (EBX/EAX restored by POPAD)
    c += b'\x53';     va[0] += 1    # PUSH EBX
    c += b'\x50';     va[0] += 1    # PUSH EAX
    c += b'\xFF\x51\x58'; va[0] += 3  # CALL [ECX+0x58]  vtable Flip
    c += b'\xE9' + rel32(va[0], RETURN_B_VA); va[0] += 5  # JMP

    assert len(c) == 93, f"cave_B size {len(c)} != 93"
    assert va[0] == CAVE_B_VA + 93
    return bytes(c)


def build_cave_e():
    """
    33-byte cave that pre-allocates the intermediate surface before
    ShowLegalScreens runs (state 0).  This ensures DAT_00496264 is non-NULL
    so the scale caves in paths C/D can stretch the legal/splash screen
    content just like menu content.

    Intercept: replaces CALL ShowLegalScreens at 0x4421E8.
    """
    c  = bytearray()
    va = [CAVE_E_VA]

    # PUSH 480; PUSH 640; CALL CreateTrackedFrontendSurface; ADD ESP,8
    c += b'\x68' + u32(GAME_H);            va[0] += 5   # PUSH 480
    c += b'\x68' + u32(GAME_W);            va[0] += 5   # PUSH 640
    c += b'\xE8' + rel32(va[0], CREATE_SURFACE_VA); va[0] += 5  # CALL
    c += b'\x83\xC4\x08';                  va[0] += 3   # ADD ESP, 8
    # MOV [DAT_00496264], EAX
    c += b'\xA3' + u32(INTER_BUF_VA);      va[0] += 5   # store surface ptr
    # CALL ShowLegalScreens
    c += b'\xE8' + rel32(va[0], SHOW_LEGAL_TARGET_VA); va[0] += 5  # CALL
    # JMP back to return address
    c += b'\xE9' + rel32(va[0], SHOW_LEGAL_RETURN_VA); va[0] += 5  # JMP

    assert len(c) == 33, f"cave_E size {len(c)} != 33"
    assert va[0] == CAVE_E_VA + 33
    return bytes(c)


ORIG_SHOW_LEGAL = bytes([0xE8, 0xF3, 0xA6, 0xFE, 0xFF])  # CALL ShowLegalScreens


# ── apply / verify ────────────────────────────────────────────────────────────

def apply():
    srcrect    = struct.pack('<4i', 0, 0, GAME_W, GAME_H)
    destrect   = struct.pack('<4i', 0, 0, W, H)
    cave_a     = build_scale_cave(CAVE_A_VA,     RETURN_A_VA, INTER_BUF_VA)
    cave_b     = build_cave_b()
    cave_c_new = build_scale_cave_c_lazy()
    cave_d     = build_scale_cave(CAVE_D_VA,     RETURN_D_VA, CAVE_INTER_VA)
    jmp_a      = b'\xE9' + rel32(INTERCEPT_A_VA, CAVE_A_VA)     + b'\x90'
    jmp_b      = b'\xE9' + rel32(INTERCEPT_B_VA, CAVE_B_VA)
    jmp_c      = b'\xE9' + rel32(INTERCEPT_C_VA, CAVE_C_NEW_VA) + b'\x90'
    jmp_d      = b'\xE9' + rel32(INTERCEPT_D_VA, CAVE_D_VA)     + b'\x90'

    with open(EXE, 'rb') as f:
        exe = bytearray(f.read())

    # --- zeroing patches -------------------------------------------------
    print("[zeroing patches]")

    def zero4(label, va):
        cur = bytes(exe[fo(va):fo(va)+4])
        if cur == b'\x00\x00\x00\x00':
            print(f"  {label}: already zero")
        else:
            exe[fo(va):fo(va)+4] = b'\x00\x00\x00\x00'
            print(f"  {label}: {cur.hex(' ')} → 00 00 00 00")

    zero4("BltFast ADD X imm", BLTFAST_ADD_X_VA)
    zero4("BltFast ADD Y imm", BLTFAST_ADD_Y_VA)
    zero4("d3d190 ADD EAX imm", D3D190_ADD_VA)

    bg_rect = struct.pack('<4i', 0, 0, GAME_W, GAME_H)
    cur = bytes(exe[fo(BLT_BG_DESTRECT):fo(BLT_BG_DESTRECT)+16])
    exe[fo(BLT_BG_DESTRECT):fo(BLT_BG_DESTRECT)+16] = bg_rect
    ok = "already correct" if cur == bg_rect else f"{cur.hex(' ')} → {bg_rect.hex(' ')}"
    print(f"  Blt bg destRect: {ok}")

    cf = exe[fo(COLORFILL_JNE_VA)]
    if cf == 0x75:
        exe[fo(COLORFILL_JNE_VA)] = 0xEB
        print(f"  COLORFILL JNE→JMP @ 0x{COLORFILL_JNE_VA:08X}")
    elif cf == 0xEB:
        print(f"  COLORFILL JMP: already set")
    else:
        print(f"  WARNING COLORFILL @ 0x{COLORFILL_JNE_VA:08X}: unexpected {hex(cf)}")

    sf = exe[fo(SPRITE_FLAG_VA) + 1]
    if sf == 0x10:
        exe[fo(SPRITE_FLAG_VA) + 1] = 0x11
        print(f"  Sprite loop flag: 0x10 → 0x11")
    elif sf == 0x11:
        print(f"  Sprite loop flag: already 0x11")
    else:
        print(f"  WARNING sprite flag @ 0x{SPRITE_FLAG_VA:08X}: {hex(sf)}")

    # --- scale rect data -------------------------------------------------
    print("\n[scale rect data]")
    exe[fo(SRCRECT_VA):fo(SRCRECT_VA)+16]   = srcrect
    exe[fo(DESTRECT_VA):fo(DESTRECT_VA)+16] = destrect
    print(f"  srcRect  @ 0x{SRCRECT_VA:08X} = {{0,0,{GAME_W},{GAME_H}}}")
    print(f"  destRect @ 0x{DESTRECT_VA:08X} = {{0,0,{W},{H}}}")

    # --- BltFast trampoline prerequisite check ---------------------------
    print("\n[prerequisite check]")
    bft_jmp = bytes(exe[fo(0x4251A9):fo(0x4251A9)+5])
    expected_bft = b'\xE9' + rel32(0x4251A9, 0x45C229)
    if bft_jmp == expected_bft:
        print(f"  BltFast tramp JMP @ 0x4251A9: OK")
    else:
        print(f"  WARNING: BltFast tramp JMP @ 0x4251A9: {bft_jmp.hex(' ')}")
        print(f"    expected: {expected_bft.hex(' ')}")
        print(f"    Run td5_widescreen.py (with centering) first if this is missing.")

    # --- caves and intercepts --------------------------------------------
    print("\n[caves and intercepts]")

    def write_cave(label, cave_va, cave, int_va, jmp, orig, tail_nops=0):
        exe[fo(cave_va):fo(cave_va)+len(cave)] = cave
        site = bytes(exe[fo(int_va):fo(int_va)+len(jmp)])
        if site == jmp:
            print(f"  cave {label} ({len(cave)}B): written  |  intercept: already JMP")
        elif site[:len(orig)] == orig:
            exe[fo(int_va):fo(int_va)+len(jmp)] = jmp
            for i in range(tail_nops):
                exe[fo(int_va) + len(jmp) + i] = 0x90
            print(f"  cave {label} ({len(cave)}B): written  |  intercept @ 0x{int_va:08X}: JMP written")
        else:
            exe[fo(int_va):fo(int_va)+len(jmp)] = jmp
            print(f"  cave {label} ({len(cave)}B): written  |  WARNING intercept @ 0x{int_va:08X}: "
                  f"unexpected {site.hex(' ')} — overwritten")

    write_cave("A",     CAVE_A_VA,     cave_a,     INTERCEPT_A_VA, jmp_a, ORIG_FLIP_IAT)
    write_cave("B",     CAVE_B_VA,     cave_b,     INTERCEPT_B_VA, jmp_b, ORIG_B_BYTES, tail_nops=2)
    write_cave("C_new", CAVE_C_NEW_VA, cave_c_new, INTERCEPT_C_VA, jmp_c, ORIG_FLIP_IAT)
    write_cave("D",     CAVE_D_VA,     cave_d,     INTERCEPT_D_VA, jmp_d, ORIG_FLIP_IAT)

    # --- surface pre-allocation cave (state 0 legal screens) ---------------
    # SKIPPED: cave_E calls CreateTrackedFrontendSurface before
    # ClearFrontendSurfaceRegistry initialises the tracking table → crash.
    # Legal screens remain 640×480 top-left (acceptable, low priority).
    print("\n[surface pre-allocation] SKIPPED (cave_E disabled)")

    # --- force-Flip presentation -------------------------------------------
    print("\n[force-Flip presentation]")
    ff = exe[fo(FORCE_FLIP_VA):fo(FORCE_FLIP_VA)+2]
    if ff == b'\x75\x08':
        exe[fo(FORCE_FLIP_VA):fo(FORCE_FLIP_VA)+2] = b'\x90\x90'
        print(f"  JNZ→NOP NOP @ 0x{FORCE_FLIP_VA:08X}")
    elif ff == b'\x90\x90':
        print(f"  Force-Flip: already NOP'd")
    else:
        print(f"  WARNING force-Flip @ 0x{FORCE_FLIP_VA:08X}: unexpected {ff.hex(' ')}")

    with open(EXE, 'wb') as f:
        f.write(exe)
    print("\nDone.")


def verify():
    srcrect    = struct.pack('<4i', 0, 0, GAME_W, GAME_H)
    destrect   = struct.pack('<4i', 0, 0, W, H)
    cave_a     = build_scale_cave(CAVE_A_VA,     RETURN_A_VA, INTER_BUF_VA)
    cave_b     = build_cave_b()
    cave_c_new = build_scale_cave_c_lazy()
    cave_d     = build_scale_cave(CAVE_D_VA,     RETURN_D_VA, CAVE_INTER_VA)
    jmp_a      = b'\xE9' + rel32(INTERCEPT_A_VA, CAVE_A_VA)     + b'\x90'
    jmp_b      = b'\xE9' + rel32(INTERCEPT_B_VA, CAVE_B_VA)
    jmp_c      = b'\xE9' + rel32(INTERCEPT_C_VA, CAVE_C_NEW_VA) + b'\x90'
    jmp_d      = b'\xE9' + rel32(INTERCEPT_D_VA, CAVE_D_VA)     + b'\x90'

    with open(EXE, 'rb') as f:
        exe = f.read()

    all_ok = True

    def chk(label, va, expected):
        nonlocal all_ok
        actual = exe[fo(va):fo(va)+len(expected)]
        ok = actual == expected
        if not ok:
            all_ok = False
        status = "OK" if ok else "MISMATCH"
        print(f"  {label:50s} [{status}]")
        if not ok:
            print(f"    expected: {expected.hex(' ')}")
            print(f"    actual:   {bytes(actual).hex(' ')}")

    print("[zeroing patches]")
    chk("BltFast ADD X imm = 0",          BLTFAST_ADD_X_VA, b'\x00\x00\x00\x00')
    chk("BltFast ADD Y imm = 0",          BLTFAST_ADD_Y_VA, b'\x00\x00\x00\x00')
    chk("d3d190 ADD EAX imm = 0",         D3D190_ADD_VA,    b'\x00\x00\x00\x00')
    chk("Blt bg destRect = {0,0,640,480}", BLT_BG_DESTRECT, struct.pack('<4i', 0, 0, GAME_W, GAME_H))

    cf = exe[fo(COLORFILL_JNE_VA)]
    cf_ok = cf == 0xEB
    if not cf_ok:
        all_ok = False
    print(f"  {'COLORFILL byte = EB (JMP)':50s} [{'OK' if cf_ok else 'MISMATCH'}]"
          f"  (actual: {hex(cf)})")

    sf = exe[fo(SPRITE_FLAG_VA) + 1]
    sf_ok = sf == 0x11
    if not sf_ok:
        all_ok = False
    print(f"  {'Sprite loop flag = 0x11':50s} [{'OK' if sf_ok else 'MISMATCH'}]"
          f"  (actual: {hex(sf)})")

    print("\n[scale rect data]")
    chk(f"srcRect  {{0,0,{GAME_W},{GAME_H}}}",  SRCRECT_VA,  srcrect)
    chk(f"destRect {{0,0,{W},{H}}}",             DESTRECT_VA, destrect)

    print("\n[caves and intercepts]")
    for label, cave_va, cave, int_va, jmp in [
        ("A",     CAVE_A_VA,     cave_a,     INTERCEPT_A_VA, jmp_a),
        ("B",     CAVE_B_VA,     cave_b,     INTERCEPT_B_VA, jmp_b),
        ("C_new", CAVE_C_NEW_VA, cave_c_new, INTERCEPT_C_VA, jmp_c),
        ("D",     CAVE_D_VA,     cave_d,     INTERCEPT_D_VA, jmp_d),
    ]:
        chk(f"cave {label} ({len(cave)}B) @ 0x{cave_va:08X}", cave_va, cave)
        chk(f"intercept {label} JMP @ 0x{int_va:08X}",        int_va,  jmp)

    print("\n[surface pre-allocation]")
    cave_e = build_cave_e()
    jmp_e  = b'\xE9' + rel32(SHOW_LEGAL_CALL_VA, CAVE_E_VA)
    chk(f"cave E ({len(cave_e)}B) @ 0x{CAVE_E_VA:08X}", CAVE_E_VA, cave_e)
    chk(f"intercept E JMP @ 0x{SHOW_LEGAL_CALL_VA:08X}", SHOW_LEGAL_CALL_VA, jmp_e)

    print("\n[force-Flip presentation]")
    ff = exe[fo(FORCE_FLIP_VA):fo(FORCE_FLIP_VA)+2]
    ff_ok = ff == b'\x90\x90'
    if not ff_ok:
        all_ok = False
    print(f"  {'Force-Flip NOP NOP @ 0x424F0C':50s} [{'OK' if ff_ok else 'MISMATCH'}]"
          f"  (actual: {ff.hex(' ')})")

    print()
    print("All OK" if all_ok else "SOME PATCHES MISSING OR MISMATCHED")


if __name__ == "__main__":
    import sys
    if "--verify" in sys.argv:
        verify()
    else:
        apply()
