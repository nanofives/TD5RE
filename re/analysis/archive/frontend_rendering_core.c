/*
 * frontend_rendering_core.c
 * ========================
 * Decompiled frontend rendering functions from TD5 executable.
 * Ghidra session: d56ed76849364e02a178a1e95dbdd171
 * Date: 2026-03-29
 *
 * 10 core functions covering: text rendering, surface management,
 * rectangle operations, gradient fills, screen navigation, and the
 * main frontend display loop.
 */


/* ============================================================================
 * 1. FUN_00424560 -- Font/Text Rendering
 *    Address: 0x424560 - 0x424655
 *    Signature: int __cdecl FUN_00424560(byte *param_1, int param_2, int param_3, int *param_4)
 *
 *    DESCRIPTION:
 *    Renders a text string (param_1) onto a DirectDraw surface (param_4) at
 *    position (param_2=x, param_3=y). Iterates through each character of the
 *    string, looks up glyph metrics from a global font table at PTR_DAT_004660c8
 *    (character widths), computes source rect coords in a font atlas (10 glyphs
 *    per row, 0x18=24px cell size), then calls the surface's Blt method
 *    (vtable offset 0x1c) to blit each glyph. The source bitmap is DAT_0049626c.
 *    If a different font mode is active (SNK_LangDLL_exref[8] != 0x30), it
 *    delegates to FUN_00424470 (alternate font renderer, likely for non-Latin).
 *    Returns total pixel width of the rendered string.
 *
 *    GLOBALS:
 *      - SNK_LangDLL_exref[8]  : font/language mode selector
 *      - PTR_DAT_004660c8      : character width table (per ASCII code)
 *      - DAT_0049626c          : font atlas surface (source for blit)
 *
 *    PARAMETERS:
 *      param_1 = text string (byte*)
 *      param_2 = x position
 *      param_3 = y position
 *      param_4 = destination DDraw surface (IDirectDrawSurface vtable ptr)
 * ============================================================================ */

int __cdecl FUN_00424560(byte *param_1,int param_2,int param_3,int *param_4)

{
  byte bVar1;
  int iVar2;
  uint uVar3;
  byte *pbVar4;
  int local_10;
  int local_c;
  int local_8;
  int local_4;

  if (SNK_LangDLL_exref[8] != (code)0x30) {
    iVar2 = FUN_00424470(param_1,param_2,param_3,param_4);
    return iVar2;
  }
  if (param_1 == (byte *)0x0) {
    return 0;
  }
  bVar1 = *param_1;
  pbVar4 = param_1;
  iVar2 = param_2;
  while (bVar1 != 0) {
    uVar3 = (uint)bVar1;
    param_1 = (byte *)(uVar3 - 0x20);
    local_c = ((uint)param_1 / 10) * 0x18;
    local_4 = local_c + 0x18;
    local_10 = ((uint)param_1 % 10) * 0x18;
    pbVar4 = pbVar4 + 1;
    local_8 = (int)*(char *)((int)&PTR_DAT_004660c8 + uVar3);
    if ('\x17' < *(char *)((int)&PTR_DAT_004660c8 + uVar3)) {
      local_8 = 0x18;
    }
    local_8 = local_8 + local_10;
    (**(code **)(*param_4 + 0x1c))(param_4,iVar2,param_3,DAT_0049626c,&local_10,0x11);
    iVar2 = iVar2 + *(char *)((int)&PTR_DAT_004660c8 + uVar3);
    bVar1 = *pbVar4;
  }
  return ((iVar2 - (char)param_1[0x4660e8]) - param_2) + 0x18;
}


/* ============================================================================
 * 2. FUN_00424a50 -- Text Width Measurement
 *    Address: 0x424a50 - 0x424ae5
 *    Signature: int __cdecl FUN_00424a50(byte *param_1, int param_2, int param_3)
 *
 *    DESCRIPTION:
 *    Computes the pixel width of a text string without rendering it.
 *    Used for centering/alignment calculations. If param_3 != 0, it
 *    computes a centered X offset: param_2 + (param_3 - totalWidth - param_2)/2.
 *    If param_3 == 0, returns the raw total pixel width.
 *    Two code paths based on SNK_LangDLL_exref[8]: one uses the font width
 *    table at PTR_DAT_004660c8 (Latin font), the other uses DAT_004662d0
 *    (alternate/localized font).
 *    Returns param_2 if the string is NULL.
 *
 *    GLOBALS:
 *      - SNK_LangDLL_exref[8]  : font mode selector
 *      - PTR_DAT_004660c8      : Latin font character width table
 *      - DAT_004662d0          : alternate font character width table
 *
 *    PARAMETERS:
 *      param_1 = text string (byte*)
 *      param_2 = x offset / left margin
 *      param_3 = container width (0 = just return raw width)
 * ============================================================================ */

int __cdecl FUN_00424a50(byte *param_1,int param_2,int param_3)

{
  byte bVar1;
  int iVar2;

  iVar2 = 0;
  if (SNK_LangDLL_exref[8] == (code)0x30) {
    if (param_1 == (byte *)0x0) {
      return param_2;
    }
    bVar1 = *param_1;
    while (bVar1 != 0) {
      param_1 = param_1 + 1;
      iVar2 = iVar2 + *(char *)((int)&PTR_DAT_004660c8 + (uint)bVar1);
      bVar1 = *param_1;
    }
    if (param_3 == 0) {
      return iVar2;
    }
  }
  else {
    if (param_1 == (byte *)0x0) {
      return param_2;
    }
    bVar1 = *param_1;
    while (bVar1 != 0) {
      param_1 = param_1 + 1;
      iVar2 = iVar2 + (char)(&DAT_004662d0)[bVar1];
      bVar1 = *param_1;
    }
    if (param_3 == 0) {
      return iVar2;
    }
  }
  return ((uint)((param_3 - iVar2) - param_2) >> 1) + param_2;
}


/* ============================================================================
 * 3. FUN_00425b60 -- Gradient Fill for Button Backgrounds
 *    Address: 0x425b60 - 0x425dd7
 *    Signature: void __cdecl FUN_00425b60(int *param_1, int param_2,
 *                                         undefined4 param_3, undefined4 param_4, int param_5)
 *
 *    DESCRIPTION:
 *    Draws a gradient-filled rectangle for button/panel backgrounds on a
 *    DirectDraw surface. param_5 is a "mode" that controls the gradient
 *    intensity (multiplied by 0x20 for color steps and 0x0C for offsets).
 *    The function draws:
 *      - A top strip using the gradient source surface (DAT_00496268)
 *      - Tiled middle rows (loop increments by 0x20 pixels at a time)
 *      - A bottom cap strip
 *      - Left and right edge columns (two loops for left/right borders)
 *      - Bottom border row and corners
 *    All blits go through the surface vtable at offset 0x1c (IDirectDrawSurface::Blt).
 *
 *    GLOBALS:
 *      - DAT_00496268 : gradient source bitmap surface
 *
 *    PARAMETERS:
 *      param_1 = destination DDraw surface
 *      param_2 = y offset
 *      param_3 = width (unused directly, but affects layout)
 *      param_4 = height (unused directly)
 *      param_5 = gradient mode / intensity level
 * ============================================================================ */

void __cdecl
FUN_00425b60(int *param_1,int param_2,undefined4 param_3,undefined4 param_4,int param_5)

{
  int iVar1;
  int iVar2;
  undefined4 ***pppuStack_58;
  int iStack_54;
  undefined4 **ppuStack_50;
  int iStack_4c;
  int iStack_48;
  undefined4 uStack_44;
  int **ppiStack_40;
  uint uStack_3c;
  int *piStack_38;
  int iStack_34;
  int iStack_30;
  undefined4 uStack_2c;
  int *piStack_28;
  int iStack_24;
  uint uVar3;
  int local_10;
  uint local_c;
  undefined4 local_8;
  int local_4;

  iStack_24 = 0x10;
  piStack_28 = &local_10;
  uStack_2c = DAT_00496268;
  iVar2 = param_5 * 0x20;
  local_c = iVar2 + 6;
  iStack_30 = param_2;
  local_4 = iVar2 + 0x13;
  iStack_34 = 0;
  piStack_38 = param_1;
  local_10 = 0;
  local_8 = 0x1a;
  uStack_3c = 0x425bad;
  (**(code **)(*param_1 + 0x1c))();
  local_10 = param_5 * 0xc;
  piStack_28 = (int *)(local_10 + 0x16);
  iStack_24 = 0x60;
  iVar1 = 0x1a;
  if (0x36 < local_c) {
    do {
      uStack_3c = 0x10;
      ppiStack_40 = &piStack_28;
      uStack_44 = DAT_00496268;
      iStack_48 = param_2;
      ppuStack_50 = (undefined4 **)param_1;
      iStack_54 = 0x425bf8;
      iStack_4c = iVar1;
      (**(code **)(*param_1 + 0x1c))();
      uVar3 = iVar1 + 0x20;
      iVar1 = iVar1 + 4;
    } while (uVar3 < local_c);
  }
  iStack_24 = iVar2 + 6;
  uStack_3c = 0x10;
  iStack_4c = local_c - 0x1c;
  ppiStack_40 = &piStack_28;
  uStack_44 = DAT_00496268;
  iStack_48 = param_2;
  ppuStack_50 = (undefined4 **)param_1;
  piStack_28 = (int *)0x1c;
  iStack_54 = 0x425c45;
  (**(code **)(*param_1 + 0x1c))();
  iStack_34 = iVar2 + 4;
  ppiStack_40 = (int **)0x0;
  piStack_38 = (int *)0x1a;
  uVar3 = 0x16;
  uStack_3c = iVar2;
  do {
    iStack_54 = 0x10;
    pppuStack_58 = &ppiStack_40;
    (**(code **)(*param_1 + 0x1c))(param_1,0,param_2 + -9 + uVar3,DAT_00496268);
    uVar3 = uVar3 + 4;
  } while (uVar3 < 0x38);
  iStack_34 = iVar2 + 4;
  ppiStack_40 = (int **)0x1c;
  piStack_38 = (int *)0x38;
  uVar3 = 0x16;
  uStack_3c = iVar2;
  do {
    iStack_54 = 0x10;
    pppuStack_58 = &ppiStack_40;
    (**(code **)(*param_1 + 0x1c))(param_1,uStack_2c,param_2 + -0xd + uVar3,DAT_00496268);
    uVar3 = uVar3 + 4;
  } while (uVar3 < 0x38);
  iStack_54 = 0x10;
  pppuStack_58 = &ppiStack_40;
  uStack_3c = iVar2 + 0x15;
  iStack_34 = iVar2 + 0x1e;
  ppiStack_40 = (int **)0x0;
  piStack_38 = (int *)0x1a;
  (**(code **)(*param_1 + 0x1c))(param_1,0,param_2 + 0x2f,DAT_00496268);
  pppuStack_58 = (undefined4 ***)(ppiStack_40 + 7);
  ppuStack_50 = ppiStack_40 + 8;
  iStack_54 = 0x60;
  iStack_4c = 100;
  iStack_34 = 0x1a;
  if (0x36 < uStack_3c) {
    do {
      (**(code **)(*param_1 + 0x1c))
                (param_1,iStack_34,param_2 + 0x34,DAT_00496268,&pppuStack_58,0x10);
      iVar1 = iStack_34 + 4;
      uVar3 = iStack_34 + 0x20;
      iStack_34 = iVar1;
    } while (uVar3 < uStack_3c);
  }
  iStack_54 = iVar2 + 0x11;
  iStack_4c = iVar2 + 0x1e;
  pppuStack_58 = (undefined4 ***)0x1c;
  ppuStack_50 = (undefined4 **)0x38;
  (**(code **)(*param_1 + 0x1c))(param_1,uStack_44,param_2 + 0x2b,DAT_00496268,&pppuStack_58,0x10);
  return;
}


/* ============================================================================
 * 4. FUN_00414610 -- Screen Navigation / Transition
 *    Address: 0x414610 - 0x414638
 *    Signature: void __cdecl FUN_00414610(int param_1)
 *
 *    DESCRIPTION:
 *    Switches the frontend to a new screen. Resets the frame counter
 *    (DAT_00495204) and scroll position (DAT_0049522c) to 0, then sets the
 *    active screen function pointer (DAT_00495238) from a jump table
 *    (PTR_LAB_004655c4) indexed by param_1. Also records the current time
 *    via timeGetTime() into DAT_004951e0 (screen entry timestamp, used for
 *    attract-mode timeout in the main loop).
 *
 *    GLOBALS:
 *      - DAT_00495204           : frame/state counter (reset to 0)
 *      - DAT_0049522c           : scroll/animation counter (reset to 0)
 *      - DAT_00495238           : active screen function pointer
 *      - PTR_LAB_004655c4       : screen function pointer jump table
 *      - DAT_004951e0           : screen entry timestamp
 *
 *    PARAMETERS:
 *      param_1 = screen index into the jump table
 * ============================================================================ */

void __cdecl FUN_00414610(int param_1)

{
  DAT_00495204 = 0;
  DAT_0049522c = 0;
  DAT_00495238 = (&PTR_LAB_004655c4)[param_1];
  DAT_004951e0 = timeGetTime();
  return;
}


/* ============================================================================
 * 5. FUN_00424c10 -- Rectangle Clear/Fill on Surface
 *    Address: 0x424c10 - 0x424c4f
 *    Signature: void __cdecl FUN_00424c10(int param_1, int param_2, int param_3, int param_4)
 *
 *    DESCRIPTION:
 *    Fills a rectangle on the active backbuffer surface (DAT_00496264) with
 *    a solid color or clear. Constructs a RECT {left=param_1, top=param_2,
 *    right=param_1+param_3, bottom=param_2+param_4} and calls FUN_004251a0
 *    (which likely wraps IDirectDrawSurface::Blt with DDBLT_COLORFILL).
 *
 *    GLOBALS:
 *      - DAT_00496264 : backbuffer / fill color surface
 *
 *    PARAMETERS:
 *      param_1 = x (left)
 *      param_2 = y (top)
 *      param_3 = width
 *      param_4 = height
 * ============================================================================ */

void __cdecl FUN_00424c10(int param_1,int param_2,int param_3,int param_4)

{
  int local_10;
  int local_c;
  int local_8;
  int local_4;

  local_8 = param_3 + param_1;
  local_4 = param_4 + param_2;
  local_10 = param_1;
  local_c = param_2;
  FUN_004251a0(param_1,param_2,DAT_00496264,&local_10,0x10);
  return;
}


/* ============================================================================
 * 6. FUN_00424c50 -- Rectangle Blit/Copy
 *    Address: 0x424c50 - 0x424c9a
 *    Signature: void __cdecl FUN_00424c50(int param_1, int param_2, int param_3, int param_4)
 *
 *    DESCRIPTION:
 *    Blits (copies) a rectangular region from a source surface (DAT_00496264)
 *    to the primary display surface (DAT_00496260). Constructs a RECT
 *    {left=param_1, top=param_2, right=param_1+param_3, bottom=param_2+param_4}
 *    and calls the surface's Blt method (vtable offset 0x1c) on DAT_00496260
 *    with DAT_00496264 as source. Flag 0x10 = DDBLT_KEYSRC or similar.
 *
 *    GLOBALS:
 *      - DAT_00496260 : primary/front display surface
 *      - DAT_00496264 : backbuffer / source surface
 *
 *    PARAMETERS:
 *      param_1 = x (left)
 *      param_2 = y (top)
 *      param_3 = width
 *      param_4 = height
 * ============================================================================ */

void __cdecl FUN_00424c50(int param_1,int param_2,int param_3,int param_4)

{
  int local_10;
  int local_c;
  int local_8;
  int local_4;

  local_8 = param_3 + param_1;
  local_4 = param_4 + param_2;
  local_10 = param_1;
  local_c = param_2;
  (**(code **)(*DAT_00496260 + 0x1c))(DAT_00496260,param_1,param_2,DAT_00496264,&local_10,0x10);
  return;
}


/* ============================================================================
 * 7. FUN_00411f00 -- Create DirectDraw Surface
 *    Address: 0x411f00 - 0x41202d
 *    Signature: undefined1* __cdecl FUN_00411f00(undefined4 param_1, undefined4 param_2)
 *
 *    DESCRIPTION:
 *    Creates an off-screen DirectDraw surface of given dimensions. Fills a
 *    DDSURFACEDESC2 struct (0x7C bytes, dwFlags=7 meaning DDSD_CAPS|DDSD_WIDTH|DDSD_HEIGHT),
 *    sets width=param_1, height=param_2, and copies pixel format from
 *    dd_exref+0x172c. Calls IDirectDraw::CreateSurface (vtable offset 0x18).
 *    If the first attempt fails and the surface caps include 0x20000000
 *    (DDSCAPS_VIDEOMEMORY), it retries (possibly without video memory requirement).
 *    On failure, logs error via DXErrorToString/Msg and sets error flag
 *    DAT_004962a4=1. On success, registers the surface in a global surface
 *    tracking table at DAT_00494fd0 (array of {ptr, id} pairs, max ~256 entries
 *    up to 0x4951d0). Increments a surface ID counter (DAT_0046549c).
 *
 *    GLOBALS:
 *      - dd_exref           : DirectDraw object/context
 *      - DAT_00494fd0       : surface tracking table (ptr array)
 *      - DAT_00494fd4       : surface tracking table (id array)
 *      - DAT_0046549c       : surface ID counter
 *      - DAT_004962a4       : error flag
 *
 *    PARAMETERS:
 *      param_1 = width
 *      param_2 = height
 *    RETURNS: pointer to the created IDirectDrawSurface, or NULL on failure
 * ============================================================================ */

/* WARNING: Globals starting with '_' overlap smaller symbols at the same address */

undefined1 * __cdecl FUN_00411f00(undefined4 param_1,undefined4 param_2)

{
  undefined4 uVar1;
  int *piVar2;
  int iVar3;
  undefined4 *puVar4;
  undefined1 *puStack_90;
  undefined4 uStack_8c;
  undefined1 local_80 [4];
  undefined4 local_7c [22];
  undefined4 uStack_24;
  undefined4 local_14;

  puVar4 = local_7c;
  for (iVar3 = 0x1f; iVar3 != 0; iVar3 = iVar3 + -1) {
    *puVar4 = 0;
    puVar4 = puVar4 + 1;
  }
  local_7c[0] = 0x7c;
  local_7c[1] = 7;
  local_14 = *(undefined4 *)(dd_exref + 0x172c);
  uStack_8c = 0;
  puStack_90 = local_80;
  local_7c[3] = param_1;
  local_7c[2] = param_2;
  iVar3 = (**(code **)(**(int **)dd_exref + 0x18))(*(int **)dd_exref,local_7c);
  if (iVar3 != 0) {
    if ((*(uint *)(dd_exref + 0x172c) & 0x20000000) != 0) {
      uStack_24 = 0x840;
      iVar3 = (**(code **)(**(int **)dd_exref + 0x18))(*(int **)dd_exref,&uStack_8c,&puStack_90,0);
    }
    if (iVar3 != 0) {
      uVar1 = DXErrorToString(iVar3);
      Msg(s_SNK_CreateSurface___d__d__Failed_004641cc,param_1,param_2,uVar1);
      _DAT_004962a4 = 1;
      return (undefined1 *)0x0;
    }
  }
  iVar3 = 0;
  piVar2 = &DAT_00494fd0;
  do {
    if (*piVar2 == 0) {
      DAT_0046549c = DAT_0046549c + 1;
      (&DAT_00494fd0)[iVar3 * 2] = puStack_90;
      if (DAT_0046549c == 0) {
        DAT_0046549c = 1;
      }
      (&DAT_00494fd4)[iVar3 * 2] = DAT_0046549c;
      return puStack_90;
    }
    piVar2 = piVar2 + 2;
    iVar3 = iVar3 + 1;
  } while ((int)piVar2 < 0x4951d0);
  Msg(s_No_Free_Surfaces__004641b8);
  return puStack_90;
}


/* ============================================================================
 * 8. FUN_00411e30 -- Release DirectDraw Surface
 *    Address: 0x411e30 - 0x411e8a
 *    Signature: undefined4 __cdecl FUN_00411e30(int *param_1)
 *
 *    DESCRIPTION:
 *    Releases a DirectDraw surface and removes it from the surface tracking
 *    table. Searches DAT_00494fd0 for the matching surface pointer, clears
 *    both the pointer and ID slots. Then calls Unlock (vtable offset 0x34,
 *    IDirectDrawSurface::Unlock) in a retry loop while the return is
 *    DDERR_WASSTILLDRAWING (0x8876021C, stored as -0x7789fde4). Finally
 *    calls Release (vtable offset 0x08) to free the COM object.
 *
 *    GLOBALS:
 *      - DAT_00494fd0 : surface tracking table (ptr slots)
 *      - DAT_00494fd4 : surface tracking table (id slots)
 *
 *    PARAMETERS:
 *      param_1 = IDirectDrawSurface pointer to release
 *    RETURNS: always 0
 * ============================================================================ */

undefined4 __cdecl FUN_00411e30(int *param_1)

{
  int *piVar1;
  int iVar2;

  if (param_1 != (int *)0x0) {
    iVar2 = 0;
    piVar1 = &DAT_00494fd0;
    while ((int *)*piVar1 != param_1) {
      piVar1 = piVar1 + 2;
      iVar2 = iVar2 + 1;
      if (0x4951cf < (int)piVar1) {
        return 0;
      }
    }
    (&DAT_00494fd0)[iVar2 * 2] = 0;
    (&DAT_00494fd4)[iVar2 * 2] = 0;
    iVar2 = (**(code **)(*param_1 + 0x34))(param_1,2);
    while (iVar2 == -0x7789fde4) {
      iVar2 = (**(code **)(*param_1 + 0x34))(param_1,2);
    }
    (**(code **)(*param_1 + 8))(param_1);
  }
  return 0;
}


/* ============================================================================
 * 9. FUN_00414b50 -- RunFrontendDisplayLoop
 *    Address: 0x414b50 - 0x414f34
 *    Signature: void __stdcall FUN_00414b50(void)
 *
 *    DESCRIPTION:
 *    The main frontend display loop tick. Called once per frame. Does:
 *
 *    1) SURFACE RECOVERY: Tests the primary surface for lost state
 *       (IDirectDrawSurface::IsLost, vtable 0x60). If lost (DDERR_SURFACELOST
 *       = 0x887601C2), calls Restore (vtable 0x6C) and sets a redraw counter
 *       (DAT_00495218=3). While redraw counter > 0, calls FUN_00424b80 to
 *       clear the screen (0x280=640, 0x1e0=480 -- 640x480 resolution).
 *
 *    2) INPUT POLLING: Reads keyboard/joystick via DXInputGetKBStick(0)
 *       and mouse via DXInput::GetMouse(). Computes rising-edge button
 *       presses in DAT_004951f8. Tracks mouse movement and sets
 *       DAT_00498714=1 if the mouse moved more than 8 pixels or a mouse
 *       button was pressed.
 *
 *    3) SCREEN DISPATCH: Calls FUN_00425170() (likely cursor/overlay update),
 *       then calls the active screen function via (*DAT_00495238)().
 *       If DAT_0049524c is set (modal dialog active?), returns early.
 *
 *    4) HUD/OVERLAY: Calls FUN_00418450() (menu indicator rendering).
 *       If mouse mode is active (DAT_00498710==1) and not in some state
 *       (DAT_0049b68c==0), draws the mouse cursor via FUN_00425660.
 *
 *    5) FRAME FLIP: Calls FUN_00425a30(), FUN_00425540() (likely
 *       double-buffer composition). If DAT_0049523c==0, does
 *       IDirectDraw::WaitForVerticalBlank + FUN_00425360 (
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *       screendump path).
 *       Otherwise calls DXDraw::Flip(1) and does a surface lock/dump.
 *       Toggles DAT_004951fc (frame parity).
 *
 *    6) POST-FRAME: Calls FUN_00426580() (likely timer/fade update).
 *       Checks for screenshot key (DXInput::CheckKey(1)).
 *       If on the Options Hub screen, processes cheat code key sequences
 *       (6 sequences tracked in DAT_004654a4/DAT_004951f0, toggling
 *       flags in PTR_DAT_00465594/DAT_004655ac with sound feedback).
 *
 *    7) ATTRACT MODE: If on the main menu (PTR_ScreenMainMenu_004655d8)
 *       and 50+ seconds have elapsed since screen entry, and some condition
 *       on DAT_0048f2fc, picks a random attract demo (0..18) and transitions
 *       to the attract/demo screen (PTR_LAB_004655cc).
 *
 *    GLOBALS (key ones):
 *      - dd_exref            : DirectDraw context
 *      - app_exref           : application context (mouse coords at +0x100, +0x104, +0x108)
 *      - DAT_00495238        : current screen function pointer
 *      - DAT_00495218        : redraw counter
 *      - DAT_00495214        : current input state
 *      - DAT_0049520c        : previous input state
 *      - DAT_004951f8        : rising-edge input (new presses)
 *      - DAT_00495258        : mouse button rising edge
 *      - DAT_00498714        : mouse-moved flag
 *      - DAT_00498710        : mouse mode enabled flag
 *      - DAT_0049523c        : windowed/screendump mode flag
 *      - DAT_004951fc        : frame parity toggle
 *      - DAT_004951e0        : screen entry timestamp
 *      - DAT_00495224        : current time
 *      - DAT_0049522c        : frame counter (incremented each tick)
 *      - DAT_00495204        : state counter
 *      - DAT_00495248        : early-return flag (modal active?)
 *      - DAT_0049524c        : post-screen early-return flag
 *      - DAT_004654a0        : screenshot target screen index
 *      - DAT_00495240        : screenshot destination
 *      - DAT_004951e8        : screenshot flag
 *      - PTR_ScreenMainMenu_004655d8 : main menu screen function
 *      - PTR_Screen_OptionsHub_004655f4 : options hub screen function
 *      - PTR_LAB_004655cc    : attract/demo screen function
 *      - DAT_004951dc        : attract mode flag
 *      - DAT_00490ba8        : random demo index
 *      - DAT_004a2c8c        : attract active flag
 *      - DAT_0048f2fc        : some game state threshold
 *      - DAT_00496280        : cursor surface
 * ============================================================================ */

/* WARNING: Globals starting with '_' overlap smaller symbols at the same address */

void FUN_00414b50(void)

{
  int iVar1;
  undefined4 uVar2;
  int iVar3;
  char *pcVar4;
  byte bVar5;
  uint uVar6;
  uint uVar7;
  int **ppiVar8;
  int *apiStack_80 [32];

  apiStack_80[0] = *(int **)(dd_exref + 4);
  iVar1 = (**(code **)(*apiStack_80[0] + 0x60))();
  if (iVar1 == -0x7789fe3e) {
    (**(code **)(**(int **)(dd_exref + 4) + 0x6c))(*(int **)(dd_exref + 4));
    DAT_00495218 = 3;
LAB_00414b8c:
    FUN_00424b80(0,0,0x280,0x1e0);
    DAT_00495218 = DAT_00495218 + -1;
  }
  else if (DAT_00495218 != 0) goto LAB_00414b8c;
  if (DAT_00495248 != 0) {
    return;
  }
  DAT_004951f8 = ~(DAT_0049520c & DAT_00495214);
  DAT_0049520c = DAT_00495214;
  DAT_00495214 = DXInputGetKBStick(0);
  DAT_004951f8 = DAT_004951f8 & DAT_0049520c & DAT_00495214;
  DAT_00495258 = ~*(uint *)(app_exref + 0x108);
  DXInput::GetMouse();
  DAT_00495258 = DAT_00495258 & *(uint *)(app_exref + 0x108);
  DAT_00498714 = 0;
  uVar6 = DAT_00495208 - *(int *)(app_exref + 0x104) >> 0x1f;
  uVar7 = DAT_00495210 - *(int *)(app_exref + 0x100) >> 0x1f;
  if ((8 < (int)(((DAT_00495208 - *(int *)(app_exref + 0x104) ^ uVar6) - uVar6) +
                ((DAT_00495210 - *(int *)(app_exref + 0x100) ^ uVar7) - uVar7))) ||
     (DAT_00495258 != 0)) {
    DAT_00498714 = 1;
  }
  if (DAT_00498710 == 1) {
    DAT_00495210 = *(int *)(app_exref + 0x100);
    DAT_00495208 = *(int *)(app_exref + 0x104);
  }
  FUN_00425170();
  (*DAT_00495238)();
  if (DAT_0049524c != 0) {
    return;
  }
  FUN_00418450();
  if ((DAT_0049b68c == 0) && (DAT_00498710 == 1)) {
    FUN_00425660(*(int *)(app_exref + 0x100),*(int *)(app_exref + 0x104),0,0,0x16,0x1e,0xff0000,
                 DAT_00496280);
  }
  FUN_00425a30();
  FUN_00425540();
  if (DAT_0049523c == 0) {
    (**(code **)(**(int **)dd_exref + 0x58))(*(int **)dd_exref,1,0);
    FUN_00425360();
  }
  else {
    DXDraw::Flip(1);
    ppiVar8 = apiStack_80;
    for (iVar1 = 0x1f; iVar1 != 0; iVar1 = iVar1 + -1) {
      *ppiVar8 = (int *)0x0;
      ppiVar8 = ppiVar8 + 1;
    }
    apiStack_80[0] = (int *)0x7c;
    iVar1 = (**(code **)(*DAT_00495220 + 100))(DAT_00495220,0,apiStack_80,1,0);
    if (iVar1 != 0) {
      uVar2 = DXErrorToString(iVar1);
      Msg(s_Lock_failed_in_SNK_ScreenDump____004658a4,uVar2);
      goto LAB_00414d87;
    }
    (**(code **)(*DAT_00495220 + 0x80))(DAT_00495220,0);
  }
  DAT_004951fc = DAT_004951fc ^ 1;
LAB_00414d87:
  FUN_00426580();
  iVar1 = DXInput::CheckKey(1);
  if ((iVar1 != 0) && (DAT_004654a0 != -1)) {
    DAT_00495240 = DAT_004654a0;
    DAT_004951e8 = 1;
  }
  if (DAT_00495238 == (code *)PTR_Screen_OptionsHub_004655f4) {
    iVar1 = 0;
    do {
      iVar3 = DXInput::CheckKey((uint)(byte)(&DAT_004654a4)
                                            [iVar1 * 0x28 + (uint)(byte)(&DAT_004951f0)[iVar1]]);
      if ((iVar3 != 0) &&
         ((&DAT_004951f0)[iVar1] = (&DAT_004951f0)[iVar1] + '\x01',
         (&DAT_004654a4)[iVar1 * 0x28 + (uint)(byte)(&DAT_004951f0)[iVar1]] == -1)) {
        *(uint *)(&PTR_DAT_00465594)[iVar1] =
             *(uint *)(&PTR_DAT_00465594)[iVar1] ^ (&DAT_004655ac)[iVar1];
        if (*(int *)(&PTR_DAT_00465594)[iVar1] == 0) {
          iVar1 = 5;
        }
        else {
          iVar1 = 4;
        }
        DXSound::Play(iVar1);
        iVar3 = DAT_004962b4;
        _DAT_004951f0 = 0;
        _DAT_004951f4 = 0;
        iVar1 = 0;
        pcVar4 = &DAT_004643b8;
        do {
          if (*pcVar4 == '\0') {
            if (iVar3 == 0) {
              bVar5 = *(byte *)((int)&DAT_004a2c9c + iVar1) & 1;
            }
            else {
              bVar5 = *(byte *)((int)&DAT_004a2c9c + iVar1) | 2;
            }
            *(byte *)((int)&DAT_004a2c9c + iVar1) = bVar5;
          }
          pcVar4 = pcVar4 + 0xa4;
          iVar1 = iVar1 + 1;
        } while ((int)pcVar4 < 0x464fe4);
      }
      iVar1 = iVar1 + 1;
    } while (iVar1 < 6);
  }
  else {
    _DAT_004951f0 = 0;
    _DAT_004951f4 = 0;
  }
  DAT_0049522c = DAT_0049522c + 1;
  if (((DAT_00495238 == (code *)PTR_ScreenMainMenu_004655d8) &&
      (DAT_00495224 = timeGetTime(), 50000 < DAT_00495224 - DAT_004951e0)) && (DAT_0048f2fc < -0xf))
  {
    do {
      iVar1 = _rand();
      DAT_00490ba8 = iVar1 % 0x13;
    } while ((&DAT_004668b0)[DAT_00490ba8] != '\0');
    DAT_004951dc = 1;
    DAT_00495204 = 0;
    DAT_0049522c = 0;
    DAT_00495238 = (code *)PTR_LAB_004655cc;
    timeGetTime();
    DAT_004951e0 = DAT_00495224;
    DAT_004a2c8c = 1;
  }
  return;
}


/* ============================================================================
 * 10. FUN_00418410 -- Load/Setup Menu Indicator Strings
 *     Address: 0x418410 - 0x418429
 *     Signature: void __cdecl FUN_00418410(int param_1, undefined4 param_2)
 *
 *     DESCRIPTION:
 *     Stores a menu indicator string/value (param_2) into a global array at
 *     offset (DAT_00496374 + param_1). Then invalidates a cached indicator
 *     index by setting DAT_00465e10 to 0xFFFFFFFF (-1), forcing a re-render
 *     of the menu indicators on the next frame.
 *
 *     GLOBALS:
 *       - DAT_00496374    : menu indicator string/value array
 *       - DAT_00465e10    : cached indicator index (-1 = invalidated)
 *
 *     PARAMETERS:
 *       param_1 = index/offset into indicator array
 *       param_2 = indicator value to store (string pointer or enum)
 * ============================================================================ */

/* WARNING: Globals starting with '_' overlap smaller symbols at the same address */

void __cdecl FUN_00418410(int param_1,undefined4 param_2)

{
  (&DAT_00496374)[param_1] = param_2;
  _DAT_00465e10 = 0xffffffff;
  return;
}


/* ============================================================================
 * SUMMARY OF KEY GLOBAL ADDRESSES
 * ============================================================================
 *
 * Surface/Display System:
 *   dd_exref          -- DirectDraw context structure
 *   app_exref         -- Application context (mouse at +0x100/+0x104/+0x108)
 *   DAT_00496260      -- Primary/front display surface (IDirectDrawSurface*)
 *   DAT_00496264      -- Backbuffer surface
 *   DAT_00496268      -- Gradient source bitmap surface
 *   DAT_0049626c      -- Font atlas surface
 *   DAT_00496280      -- Mouse cursor surface
 *   DAT_00494fd0      -- Surface tracking table (pointers, stride=8)
 *   DAT_00494fd4      -- Surface tracking table (IDs, stride=8)
 *   DAT_0046549c      -- Surface ID counter
 *   DAT_004962a4      -- Surface creation error flag
 *   DAT_00495220      -- Screen dump surface
 *
 * Font System:
 *   SNK_LangDLL_exref -- Language DLL reference; [8] selects font mode
 *   PTR_DAT_004660c8  -- Latin font character width table (per ASCII byte)
 *   DAT_004662d0      -- Alternate font character width table
 *
 * Frontend State Machine:
 *   DAT_00495238      -- Active screen function pointer (called each frame)
 *   PTR_LAB_004655c4  -- Screen function pointer jump table
 *   PTR_ScreenMainMenu_004655d8    -- Main menu screen function
 *   PTR_Screen_OptionsHub_004655f4 -- Options hub screen function
 *   PTR_LAB_004655cc  -- Attract/demo screen function
 *   DAT_00495204      -- State counter (reset on screen transition)
 *   DAT_0049522c      -- Frame counter (incremented each tick, reset on transition)
 *   DAT_004951e0      -- Screen entry timestamp (from timeGetTime)
 *   DAT_00495224      -- Current timestamp
 *   DAT_00495248      -- Modal/early-return flag
 *   DAT_0049524c      -- Post-screen early-return flag
 *
 * Input System:
 *   DAT_00495214      -- Current keyboard/joystick state
 *   DAT_0049520c      -- Previous keyboard/joystick state
 *   DAT_004951f8      -- Rising-edge input (newly pressed buttons)
 *   DAT_00495258      -- Mouse button rising edge
 *   DAT_00498714      -- Mouse-moved flag
 *   DAT_00498710      -- Mouse mode enabled
 *   DAT_00495210      -- Last mouse X
 *   DAT_00495208      -- Last mouse Y
 *
 * Display:
 *   DAT_00495218      -- Redraw counter (3 on surface restore)
 *   DAT_0049523c      -- Windowed/screendump mode flag
 *   DAT_004951fc      -- Frame parity toggle (XOR'd each frame)
 *
 * Cheat/Debug:
 *   DAT_004654a0      -- Screenshot target screen
 *   DAT_00495240      -- Screenshot destination
 *   DAT_004951e8      -- Screenshot request flag
 *   DAT_004654a4      -- Cheat code key sequence table (6 sequences, 0x28 stride)
 *   DAT_004951f0      -- Cheat code progress counters (6 bytes)
 *   PTR_DAT_00465594  -- Cheat toggle target addresses
 *   DAT_004655ac      -- Cheat toggle XOR masks
 *   DAT_004962b4      -- Some game state flag
 *
 * Menu Indicators:
 *   DAT_00496374      -- Menu indicator value array
 *   DAT_00465e10      -- Cached indicator index (-1 = invalidated)
 *
 * Attract Mode:
 *   DAT_004951dc      -- Attract mode flag
 *   DAT_00490ba8      -- Random demo index (0..18)
 *   DAT_004668b0      -- Demo availability flags
 *   DAT_004a2c8c      -- Attract active flag
 *   DAT_0048f2fc      -- Game state threshold for attract trigger
 * ============================================================================ */
