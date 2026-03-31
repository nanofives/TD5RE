// =============================================================================
// TD5 Frontend Functions - Decompiled from TD5_d3d.exe
// Decompiled via Ghidra MCP, 2026-03-29
// =============================================================================

// =============================================================================
// 1. FUN_00425de0 -- Button Creation (create a button surface with text)
// =============================================================================
// Signature: int* __cdecl FUN_00425de0(byte *param_1, int param_2, int param_3,
//                                      uint param_4, int param_5, undefined4 param_6)
//
// Parameters:
//   param_1 = text string (byte*) to render on the button
//   param_2 = X position on screen
//   param_3 = Y position on screen
//   param_4 = button width
//   param_5 = button height
//   param_6 = user data / callback ID stored in button slot
//
// Globals read:
//   DAT_0049b694  -- flag: 0 = "double height mode" (normal buttons), nonzero = halfbrite mode
//   DAT_0049525c  -- pixel format depth (0xf=15-bit, 0x10=16-bit)
//   DAT_00499c88  -- button slot array sentinel (-1 = free slot)
//   DAT_0049a978  -- reset to 0 when button is created
//
// Globals written:
//   DAT_00499c78..DAT_00499ca8 -- button descriptor array (stride 0xd dwords = 0x34 bytes)
//     [+0x00] x1       [+0x04] y1       [+0x08] x2       [+0x0C] y2
//     [+0x10] srcX     [+0x14] srcY     [+0x18] srcW     [+0x1C] srcH
//     [+0x20] userData [+0x24] surface  [+0x28] ddSurface [+0x2C] state [+0x30] flags
//   DAT_0049a978 -- tick counter (reset to 0)
//
// What it does:
//   Creates a DirectDraw surface for a button. In normal mode (DAT_0049b694==0),
//   it allocates a double-height surface to hold both "up" and "down" button states,
//   fills them with FUN_00425b60 (gradient fill), then renders the text string onto
//   both halves. In halfbrite mode, it creates a single-height surface and applies a
//   pixel-level half-brightness effect (shifting colors right by 1 and masking).
//   Finally it finds a free slot in the button array (up to ~38 buttons at
//   0x499c78..0x49a988) and stores position, source rect, surface pointer, and state.
//   Returns the DDraw surface pointer, or NULL if no free slot.

int * __cdecl
FUN_00425de0(byte *param_1,int param_2,int param_3,uint param_4,int param_5,undefined4 param_6)
{
  short sVar1;
  int *piVar2;
  uint uVar3;
  int iVar4;
  uint uVar5;
  int *piVar6;
  undefined4 uVar7;
  uint *puVar8;
  undefined4 *puVar9;
  bool bVar10;
  int iVar11;
  undefined4 local_108 [4];
  int local_f8;
  uint *local_e4;
  undefined4 local_8c [20];
  undefined4 local_3c;
  uint *local_28;
  int local_24;
  int local_20;
  uint local_1c;
  undefined4 local_18;
  int local_14;
  uint local_10;
  int local_c;
  int *local_8;

  iVar4 = param_5;
  if (DAT_0049b694 == 0) {
    iVar4 = param_5 * 2;
  }
  piVar2 = (int *)FUN_00411f00(param_4,iVar4);
  local_3c = 0;
  local_18 = 0;
  local_14 = 0;
  local_8c[0] = 100;
  local_10 = param_4;
  local_c = param_5;
  local_8 = piVar2;
  (**(code **)(*piVar2 + 0x14))(piVar2,&local_18,0,0,0x400,local_8c);
  if (DAT_0049b694 == 0) {
    if (DAT_0049525c == 0xf) {
      local_3c = 0x1c8a;
    }
    else if (DAT_0049525c == 0x10) {
      local_3c = 0x390a;
    }
    local_14 = local_14 + param_5;
    local_c = local_c + param_5;
    (**(code **)(*piVar2 + 0x14))(piVar2,&local_18,0,0,0x400,local_8c);
    FUN_00425b60(piVar2,0,param_4,param_5,1);
    FUN_00425b60(piVar2,param_5,param_4,param_5,0);
  }
  else {
    FUN_00425b60(piVar2,0,param_4,param_5,2);
  }
  if ((param_1 != (byte *)0x0) && (uVar3 = FUN_00424a50(param_1,0,0), uVar3 <= param_4)) {
    iVar11 = 0;
    piVar6 = piVar2;
    iVar4 = FUN_00424a50(param_1,0,param_4);
    FUN_00424560(param_1,iVar4,iVar11,piVar6);
    if (DAT_0049b694 == 0) {
      iVar4 = param_5;
      piVar6 = piVar2;
      iVar11 = FUN_00424a50(param_1,0,param_4);
      FUN_00424560(param_1,iVar11,iVar4,piVar6);
    }
    else {
      if (DAT_0049525c == 0x10) {
        local_1c = 0x7bcf7bcf;
        param_1 = (byte *)0x2104;
      }
      else if (DAT_0049525c == 0xf) {
        local_1c = 0x3def3def;
        param_1 = (byte *)0x1084;
      }
      puVar9 = local_108;
      for (iVar4 = 0x1f; iVar4 != 0; iVar4 = iVar4 + -1) {
        *puVar9 = 0;
        puVar9 = puVar9 + 1;
      }
      local_108[0] = 0x7c;
      iVar4 = (**(code **)(*piVar2 + 100))(piVar2,0,local_108,1,0);
      if (iVar4 == 0) {
        local_24 = local_f8;
        local_28 = local_e4;
        local_20 = (int)param_4 / 2;
        iVar4 = local_20;
        puVar8 = local_e4;
        iVar11 = param_5;
        do {
          do {
            uVar3 = *local_e4 >> 1 & local_1c;
            if ((short)uVar3 != 0) {
              uVar3 = CONCAT22((short)(uVar3 >> 0x10),(short)uVar3 + (short)param_1);
            }
            uVar5 = uVar3 << 0x10 | uVar3 >> 0x10;
            sVar1 = (short)(uVar3 >> 0x10);
            if (sVar1 != 0) {
              uVar5 = CONCAT22((short)uVar3,sVar1 + (short)param_1);
            }
            *local_e4 = uVar5 << 0x10 | uVar5 >> 0x10;
            local_e4 = local_e4 + 1;
            iVar4 = iVar4 + -1;
          } while (iVar4 != 0);
          local_e4 = (uint *)((int)puVar8 + local_f8);
          iVar11 = iVar11 + -1;
          iVar4 = local_20;
          puVar8 = local_e4;
        } while (iVar11 != 0);
        (**(code **)(*local_8 + 0x80))(local_8,0);
        piVar2 = local_8;
      }
      else {
        uVar7 = DXErrorToString(iVar4);
        Msg(s_Lock_failed_in_SNK_HalfBriteSurf_00466704,uVar7);
      }
    }
  }
  iVar4 = 0;
  piVar6 = &DAT_00499c88;
  do {
    if (*piVar6 == -1) {
      (&DAT_00499c78)[iVar4 * 0xd] = param_2;
      (&DAT_00499c7c)[iVar4 * 0xd] = param_3;
      (&DAT_00499c80)[iVar4 * 0xd] = param_2 + param_4;
      (&DAT_00499c84)[iVar4 * 0xd] = param_3 + param_5;
      (&DAT_00499c88)[iVar4 * 0xd] = 0;
      (&DAT_00499c8c)[iVar4 * 0xd] = 0;
      (&DAT_00499c90)[iVar4 * 0xd] = param_4;
      (&DAT_00499c94)[iVar4 * 0xd] = param_5;
      (&DAT_00499c98)[iVar4 * 0xd] = param_6;
      (&DAT_00499c9c)[iVar4 * 0xd] = piVar2;
      uVar7 = FUN_00411e00((int)piVar2);
      (&DAT_00499ca0)[iVar4 * 0xd] = uVar7;
      bVar10 = DAT_0049b694 != 0;
      *(undefined4 *)(&DAT_00499ca4 + iVar4 * 0x34) = 1;
      if (bVar10) {
        *(undefined4 *)(&DAT_00499ca4 + iVar4 * 0x34) = 5;
      }
      (&DAT_00499ca8)[iVar4 * 0xd] = 0;
      DAT_0049a978 = 0;
      return piVar2;
    }
    piVar6 = piVar6 + 0xd;
    iVar4 = iVar4 + 1;
  } while ((int)piVar6 < 0x49a988);
  Msg(s_Out_of_Buttons__0046672c);
  FUN_00411e30(piVar2);
  return (int *)0x0;
}


// =============================================================================
// 2. FUN_004259d0 -- Position/Move a Button
// =============================================================================
// Signature: void __cdecl FUN_004259d0(int param_1, undefined4 param_2, undefined4 param_3)
//
// Parameters:
//   param_1 = button index into the button array
//   param_2 = new X position
//   param_3 = new Y position
//
// Globals read/written:
//   DAT_00499c78..DAT_00499c84 -- button rect fields (x1, y1, x2, y2) at stride 0xd dwords
//
// What it does:
//   Moves a button to a new screen position. It computes the button's width and height
//   by subtracting (x1,y1) from (x2,y2), sets (x1,y1) to the new position, then
//   recomputes (x2,y2) = new position + width/height. Classic "move rect" operation.

void __cdecl FUN_004259d0(int param_1,undefined4 param_2,undefined4 param_3)
{
  (&DAT_00499c80)[param_1 * 0xd] = (&DAT_00499c80)[param_1 * 0xd] - (&DAT_00499c78)[param_1 * 0xd];
  (&DAT_00499c84)[param_1 * 0xd] = (&DAT_00499c84)[param_1 * 0xd] - (&DAT_00499c7c)[param_1 * 0xd];
  (&DAT_00499c78)[param_1 * 0xd] = param_2;
  (&DAT_00499c7c)[param_1 * 0xd] = param_3;
  (&DAT_00499c80)[param_1 * 0xd] = (&DAT_00499c80)[param_1 * 0xd] + (&DAT_00499c78)[param_1 * 0xd];
  (&DAT_00499c84)[param_1 * 0xd] = (&DAT_00499c84)[param_1 * 0xd] + (&DAT_00499c7c)[param_1 * 0xd];
  return;
}


// =============================================================================
// 3. FUN_00425660 -- Draw/Blit a Sprite to the Screen
// =============================================================================
// Signature: void __cdecl FUN_00425660(int param_1, int param_2, int param_3, int param_4,
//                                      int param_5, int param_6, undefined4 param_7, int param_8)
//
// Parameters:
//   param_1 = dest X on screen
//   param_2 = dest Y on screen
//   param_3 = source X in surface (also used as validity sentinel; <0 means skip)
//   param_4 = source Y in surface
//   param_5 = width to blit
//   param_6 = height to blit
//   param_7 = flags/blit mode
//   param_8 = DDraw surface pointer (source surface)
//
// Globals read/written:
//   DAT_00498718  -- sprite queue count (index of next free slot)
//   DAT_00498f40..DAT_00498f68 -- sprite queue array (stride 0xd dwords = 0x34 bytes)
//     [+0x00] destX    [+0x04] destY    [+0x08] destX2   [+0x0C] destY2
//     [+0x10] srcX     [+0x14] srcY     [+0x18] srcX2    [+0x1C] srcY2
//     [+0x20] flags    [+0x24] surface  [+0x28] ddSurface
//
// What it does:
//   Queues a sprite blit into the sprite render list. Stores dest rect
//   (x, y, x+w, y+h), source rect, flags, and surface pointer. Increments the
//   queue counter. Max 64 sprites; prints "FE: Out of Sprites" if exceeded.
//   Marks the next slot's srcX as -1 as a terminator.

void __cdecl
FUN_00425660(int param_1,int param_2,int param_3,int param_4,int param_5,int param_6,
            undefined4 param_7,int param_8)
{
  int iVar1;
  undefined4 uVar2;
  int iVar3;

  iVar3 = DAT_00498718;
  if (-1 < param_3) {
    iVar1 = DAT_00498718 * 0x34;
    (&DAT_00498f40)[DAT_00498718 * 0xd] = param_1;
    (&DAT_00498f44)[iVar3 * 0xd] = param_2;
    *(int *)(&DAT_00498f48 + iVar1) = param_1 + param_5;
    *(int *)(&DAT_00498f4c + iVar1) = param_2 + param_6;
    (&DAT_00498f50)[iVar3 * 0xd] = param_3;
    (&DAT_00498f54)[iVar3 * 0xd] = param_4;
    (&DAT_00498f58)[iVar3 * 0xd] = param_3 + param_5;
    (&DAT_00498f5c)[iVar3 * 0xd] = param_4 + param_6;
    (&DAT_00498f60)[iVar3 * 0xd] = param_7;
    (&DAT_00498f64)[iVar3 * 0xd] = param_8;
    uVar2 = FUN_00411e00(param_8);
    iVar3 = DAT_00498718 + 1;
    (&DAT_00498f68)[DAT_00498718 * 0xd] = uVar2;
    if (0x40 < iVar3) {
      Msg(s_FE__Out_of_Sprites__004666f0);
      return;
    }
    DAT_00498718 = iVar3;
    (&DAT_00498f50)[iVar3 * 0xd] = 0xffffffff;
  }
  return;
}


// =============================================================================
// 4. FUN_004183b0 -- Load Menu Title/Indicator Strings
// =============================================================================
// Signature: void __cdecl FUN_004183b0(char *param_1, undefined4 param_2, undefined4 param_3)
//
// Parameters:
//   param_1 = packed null-terminated string list (e.g. "Title1\0Title2\0\0")
//   param_2 = stored at DAT_004963f8 (likely X position or callback)
//   param_3 = stored at DAT_004963fc (likely Y position or callback)
//
// Globals read/written:
//   DAT_004963f4  -- count of parsed strings
//   DAT_00496374  -- array of string pointers (up to address 0x4963f4, i.e. ~32 entries)
//   DAT_00465e10  -- set to -1 (likely "no current selection" sentinel)
//   DAT_004963f8  -- param_2
//   DAT_004963fc  -- param_3
//
// What it does:
//   Parses a packed multi-string buffer (each string is null-terminated, the list
//   ends with a double-null) into an array of string pointers at DAT_00496374.
//   Counts the strings into DAT_004963f4. Resets DAT_00465e10 to -1 (no selection).
//   Stores param_2 and param_3 as menu metadata. Used to set up menu title/item lists.

void __cdecl FUN_004183b0(char *param_1,undefined4 param_2,undefined4 param_3)
{
  char cVar1;
  undefined4 *puVar2;

  _DAT_004963f4 = 0;
  puVar2 = &DAT_00496374;
  do {
    _DAT_004963f4 = _DAT_004963f4 + 1;
    *puVar2 = param_1;
    cVar1 = *param_1;
    while (param_1 = param_1 + 1, cVar1 != '\0') {
      cVar1 = *param_1;
    }
  } while ((*param_1 != '\0') && (puVar2 = puVar2 + 1, (int)puVar2 < 0x4963f4));
  _DAT_00465e10 = 0xffffffff;
  _DAT_004963f8 = param_2;
  _DAT_004963fc = param_3;
  return;
}


// =============================================================================
// 5. FUN_004258c0 -- Setup Cursor/Input Ready
// =============================================================================
// Signature: void FUN_004258c0(void)
//
// Parameters: none
//
// Globals written:
//   DAT_0049b68c = 1  -- cursor/input enabled flag
//   DAT_00498714 = 0  -- likely mouse click state or input event counter (reset)
//
// What it does:
//   Enables cursor/input handling by setting the "input ready" flag to 1 and
//   resetting the input event counter to 0. Called at menu setup to activate
//   user interaction.

void FUN_004258c0(void)
{
  DAT_0049b68c = 1;
  DAT_00498714 = 0;
  return;
}


// =============================================================================
// 6. FUN_004259b0 -- Check Input/Tick Ready
// =============================================================================
// Signature: bool FUN_004259b0(void)
//
// Parameters: none
//
// Globals read/written:
//   DAT_0049a978 -- tick counter (incremented each call)
//
// What it does:
//   Increments a global tick counter and returns true if it exceeds 2.
//   This is a debounce/delay mechanism -- after a button is created (which
//   resets DAT_0049a978 to 0), this function must be called at least 3 times
//   before it returns true, preventing immediate accidental clicks on newly
//   created buttons.

bool FUN_004259b0(void)
{
  DAT_0049a978 = DAT_0049a978 + 1;
  return 2 < DAT_0049a978;
}


// =============================================================================
// 7. FUN_00424af0 -- Present Buffer (flip/blit back buffer to screen)
// =============================================================================
// Signature: void FUN_00424af0(void)
//
// Parameters: none
//
// Globals read:
//   DAT_00495228 -- screen width (e.g. 640)
//   DAT_00495200 -- screen height (e.g. 480)
//   DAT_00496260 -- back buffer DDraw surface pointer
//
// What it does:
//   Presents the back buffer to the screen. Builds a RECT {0, 0, width, height}
//   and calls FUN_004251a0 (which performs the actual Blt/Flip from back buffer
//   to primary surface) with flags 0x10. This is the final step in each frame's
//   render cycle.

void FUN_00424af0(void)
{
  int local_10 [3];
  undefined4 local_4;

  local_10[2] = DAT_00495228;
  local_4 = DAT_00495200;
  local_10[0] = 0;
  local_10[1] = 0;
  FUN_004251a0(0,0,DAT_00496260,local_10,0x10);
  return;
}


// =============================================================================
// 8. FUN_00412e30 -- Load a Surface/Image (menu sprite/indicator graphic)
// =============================================================================
// Signature: int* __cdecl FUN_00412e30(int param_1)
//
// Parameters:
//   param_1 = index into SNK_MenuStrings_exref (string table for menu sprite names)
//
// Globals read:
//   SNK_LangDLL_exref[8] -- if not 0x30, uses string-based rendering path
//   SNK_MenuStrings_exref -- string table (array of char* pointers)
//
// Globals written:
//   DAT_004962c4 -- loaded sprite width (string path) or string "SelectCompCarText" (TGA path)
//   DAT_004962c8 -- loaded sprite height or 0x14
//   DAT_004962cc -- color depth info: 0x10 (string path) or 0x0 (TGA path)
//
// What it does:
//   Loads a menu sprite/indicator graphic. Two code paths:
//
//   Path A (SNK_LangDLL_exref[8] != 0x30): String-based rendering.
//     Looks up a string from the menu strings table by index, measures its pixel
//     width via FUN_00412d50, creates a 36-pixel-tall surface via FUN_00411f00,
//     clears it via FUN_00424050, then renders the text onto it with FUN_00412d50.
//     Stores the width, 0x24 height, and 0x10 color info.
//
//   Path B (TGA file loading): Builds a filename like "Front End/<name>.tga",
//     loads it from "Front End/FrontEnd.zip" via FUN_00412030. Locks the surface
//     to read pixel data, stores metadata, then unlocks. Returns the surface
//     pointer or NULL on failure.

int * __cdecl FUN_00412e30(int param_1)
{
  int *piVar1;
  undefined4 uVar2;
  int iVar3;
  undefined4 *puVar4;
  undefined4 local_fc [31];
  CHAR local_80 [128];

  if (SNK_LangDLL_exref[8] != (code)0x30) {
    iVar3 = FUN_00412d50(*(byte **)(SNK_MenuStrings_exref + param_1 * 4),0,0,(int *)0x0);
    if (iVar3 == 0) {
      return (int *)0x0;
    }
    piVar1 = (int *)FUN_00411f00(iVar3,0x24);
    FUN_00424050(0,0,0,iVar3,0x24,piVar1);
    FUN_00412d50(*(byte **)(SNK_MenuStrings_exref + param_1 * 4),0,0,piVar1);
    DAT_004962c4 = (char *)iVar3;
    DAT_004962c8 = 0x24;
    DAT_004962cc = 0x10;
    return piVar1;
  }
  FUN_00448443(local_80,(byte *)s_Front_End__s_tga_004642b4);
  piVar1 = FUN_00412030(local_80,s_Front_End_FrontEnd_zip_00463f84);
  if (piVar1 == (int *)0x0) {
    return (int *)0x0;
  }
  puVar4 = local_fc;
  for (iVar3 = 0x1f; iVar3 != 0; iVar3 = iVar3 + -1) {
    *puVar4 = 0;
    puVar4 = puVar4 + 1;
  }
  local_fc[0] = 0x7c;
  iVar3 = (**(code **)(*piVar1 + 100))(piVar1,0,local_fc,1,0);
  if (iVar3 != 0) {
    uVar2 = DXErrorToString(iVar3);
    Msg(s_Lock_failed_in_SNK_GetMenuSprite_0046428c,uVar2);
    return (int *)0x0;
  }
  DAT_004962c4 = s_SelectCompCarText_004642d4;
  DAT_004962c8 = 0x14;
  DAT_004962cc = 0;
  (**(code **)(*piVar1 + 0x80))(piVar1,0);
  return piVar1;
}
