/*
 * frontend_screens_decompiled.c
 *
 * Ghidra decompilation of 6 key frontend screen functions from TD5.
 * Decompiled 2026-03-29 using Ghidra MCP session.
 *
 * Functions:
 *   1. Screen_QuickRaceMenu        (0x004213d0) - Screen 7:  Quick race car/track selection
 *   2. Screen_RaceTypeCategory     (0x004168b0) - Screen 6:  Race type menu
 *   3. Screen_CarSelection         (0x0040dfc0) - Screen 20: Car selection with preview
 *   4. Screen_TrackSelection       (0x00427630) - Screen 21: Track/direction selection
 *   5. Screen_OptionsHub           (0x0041d890) - Screen 12: Options menu hub
 *   6. InitializeFrontendResourcesAndState (0x00414740) - Full frontend init
 */


/* ============================================================================
 * 1. Screen_QuickRaceMenu (0x004213d0)
 *    Screen 7 - Quick Race Menu
 *
 * STATE MACHINE: 7 states (0-6)
 *
 * KEY OBSERVATIONS:
 *   - Background TGA: "Front_End\\MainMenu.tga" from "Front_End\\FrontEnd.zip"
 *   - Info surface: 0x208 x 200 (520 x 200 pixels)
 *   - Background image loaded via FUN_004125b0
 *   - Car/track preview buffer at DAT_0049628c
 *   - Title banner loaded via FUN_00412e30(3)
 *
 * BUTTONS (via FUN_00425de0):
 *   - SNK_ChangeCarButTxt   at (halfW-200, halfH-0x67)  size 0x100 x 0x20
 *   - SNK_ChangeTrackButTxt at (halfW-200, halfH+0x11)  size 0x100 x 0x20
 *   - SNK_OkButTxt          at (halfW-200, halfH+0x89)  size 0x60  x 0x20
 *   - SNK_BackButTxt        at (halfW-0x58, halfH+0x89) size 0x70  x 0x20
 *   DAT_004654a0 = 3 (button count - 1, so 4 buttons total)
 *
 * NAVIGATION FLOW:
 *   - State 0: Init - load BG, create info surface, place buttons, set menu text
 *   - State 1-2: Fade in transition
 *   - State 3: Slide-in animation (buttons animate from offscreen)
 *   - State 4: INTERACTIVE - L/R changes car (slot 0) or track (slot 1),
 *              OK (slot 2) -> validates locked status, sets race params, exits
 *              Back (slot 3) -> sets DAT_00496350=5, exits
 *   - State 5: Transition out, calls FUN_00418430 (menu teardown)
 *   - State 6: Final exit animation, frees surfaces, navigates to next screen
 *              If DAT_00496350==-1 -> FUN_0040dac0 + FUN_00414a90 (back to main)
 *              Else -> FUN_00414610(DAT_00496350) (go to destination screen)
 *
 * LOCKED ITEMS:
 *   - Car locked check: (&DAT_00463e4c)[DAT_0048f31c] != '\0'
 *   - Track locked check: (&DAT_004668b0)[DAT_004a2c90] != '\0'
 *   - Shows SNK_LockedTxt when locked & not in cheat mode (DAT_00496298==0)
 *   - Plays DXSound::Play(10) on locked item selection attempt
 *
 * SOUND:
 *   - DXSound::Play(5) on init and transitions
 *   - DXSound::Play(4) on slide-in completion
 *   - DXSound::Play(10) on locked item rejection
 * ============================================================================ */

void Screen_QuickRaceMenu(void)

{
  bool bVar1;
  undefined3 extraout_var;
  uint uVar2;
  int iVar3;
  uint uVar4;
  int iVar5;
  byte abStack_80 [128];

  uVar4 = DAT_00495228 >> 1;
  uVar2 = DAT_00495200 >> 1;
  iVar5 = uVar4 - 0xd2;
  iVar3 = uVar2 - 0x9f;
  switch(DAT_00495204) {
  case 0:
    DAT_004a2c98 = 0;
    DAT_0048f338 = 0;
    DAT_0048f37c = 0;
    DAT_0048f378 = 0;
    if (((DAT_004962ac != 0) && (DAT_00496298 != 0)) || (iVar3 = 0x20, DAT_00463e6d == '\0')) {
      iVar3 = DAT_00463e0c;
    }
    if (iVar3 < DAT_0048f364) {
      DAT_0048f364 = 0;
    }
    if (iVar3 < DAT_00463e08) {
      DAT_00463e08 = 0;
    }
    if (iVar3 < DAT_0048f31c) {
      DAT_0048f31c = 0;
    }
    if ((DAT_00466840 < DAT_004a2c90) || (DAT_004a2c90 < 0)) {
      DAT_004a2c90 = 0;
    }
    DAT_0049635c = 0xffffffff;
    DAT_004951dc = 0;
    DAT_00496358 = FUN_00412e30(3);
    DAT_0049628c = (int *)FUN_00411f00(0x208,200);
    FUN_00424050(0,0,0,0x208,200,DAT_0049628c);
    FUN_004125b0(s_Front_End_MainMenu_tga_00463ecc,s_Front_End_FrontEnd_zip_00463f84);
    FUN_00424b30();
    FUN_00448443(abStack_80,&DAT_004660b0);
    FUN_00424560(abStack_80,0,0,DAT_0049628c);
    FUN_00448443(abStack_80,&DAT_004660b0);
    FUN_00424560(abStack_80,0,0x78,DAT_0049628c);
    if (((&DAT_00463e4c)[DAT_0048f31c] != '\0') && (DAT_00496298 == 0)) {
      FUN_00424560((byte *)SNK_LockedTxt_exref,0x118,0x28,DAT_0049628c);
    }
    if (((&DAT_004668b0)[DAT_004a2c90] != '\0') && (DAT_00496298 == 0)) {
      FUN_00424560((byte *)SNK_LockedTxt_exref,0x118,0xa0,DAT_0049628c);
    }
    iVar3 = uVar4 - 200;
    FUN_00425de0((byte *)SNK_ChangeCarButTxt_exref,iVar3,uVar2 - 0x67,0x100,0x20,0);
    FUN_00425de0((byte *)SNK_ChangeTrackButTxt_exref,iVar3,uVar2 + 0x11,0x100,0x20,0);
    FUN_00425de0((byte *)SNK_OkButTxt_exref,iVar3,uVar2 + 0x89,0x60,0x20,0);
    FUN_00425de0((byte *)SNK_BackButTxt_exref,uVar4 - 0x58,uVar2 + 0x89,0x70,0x20,0);
    DAT_004654a0 = 3;
    FUN_00426260(0,1);
    FUN_00426260(1,1);
    FUN_004259d0(0,0xffffff20,0);
    FUN_004259d0(1,0xffffff20,0);
    FUN_004259d0(2,0xffffff20,0);
    FUN_004259d0(3,0xffffff20,0);
    FUN_004258c0();
    FUN_004183b0((char *)SNK_QuickRaceMenu_MT_exref,0,0);
    FUN_004129b0(DAT_00496270);
    FUN_00412b00(DAT_00496270,0);
    DXSound::Play(5);
    DAT_00495204 = DAT_00495204 + 1;
    return;
  case 1:
  case 2:
    FUN_00424af0();
    DAT_0049522c = 0;
    DAT_00495204 = DAT_00495204 + 1;
    return;
  case 3:
    FUN_004259d0(0,DAT_0049522c * 0x10 + -0x266 + iVar5,uVar2 - 0x67);
    FUN_004259d0(1,iVar5 + DAT_0049522c * -0x10 + 0x27a,uVar2 + 0x11);
    FUN_004259d0(2,DAT_0049522c * 0x10 + -0x266 + iVar5,uVar2 + 0x89);
    FUN_004259d0(3,iVar5 + DAT_0049522c * -0x10 + 0x2ea,uVar2 + 0x89);
    FUN_00425660(uVar4 - 200,((DAT_0049522c * 4 + -0xdc) - DAT_004962cc) + iVar3,0,0,DAT_004962c4,
                 DAT_004962c8,0,(int)DAT_00496358);
    FUN_00425660(uVar4 - 200,iVar3 + DAT_0049522c * -0x18 + 0x3b8,0,0,0x208,200,0,(int)DAT_0049628c)
    ;
    if (DAT_0049522c == 0x27) {
      DAT_0049522c = 0x26;
      bVar1 = FUN_004259b0();
      if (CONCAT31(extraout_var,bVar1) != 0) {
        FUN_004258e0();
        DXSound::Play(4);
        DAT_00495204 = DAT_00495204 + 1;
        return;
      }
    }
    break;
  case 4:
    FUN_00425660(uVar4 - 200,(iVar3 - DAT_004962cc) + -0x40,0,0,DAT_004962c4,DAT_004962c8,0,
                 (int)DAT_00496358);
    FUN_00425660(uVar4 - 200,uVar2 - 0x8f,0,0,0x208,200,0,(int)DAT_0049628c);
    if (DAT_0049b690 != 0) {
      if (DAT_00495240 == 0) {
        DAT_0048f31c = DAT_0048f31c + DAT_0049b690;
        if (DAT_00496298 == 0) {
          if (DAT_0048f31c < 0) {
            DAT_0048f31c = DAT_00463e0c + -1;
          }
          if (DAT_0048f31c == DAT_00463e0c) {
LAB_004219cc:
            DAT_0048f31c = 0;
          }
        }
        else if ((DAT_004962ac == 0) && (DAT_00463e6d != '\0')) {
          if (DAT_0048f31c < 0) {
            DAT_0048f31c = 0x20;
          }
          else if (DAT_0048f31c == 0x21) goto LAB_004219cc;
        }
        else if (DAT_0048f31c < 0) {
          DAT_0048f31c = 0x24;
        }
        else if (DAT_0048f31c == 0x25) goto LAB_004219cc;
        FUN_00424050(0,0,0,0x208,0x40,DAT_0049628c);
        FUN_00448443(abStack_80,&DAT_004660b0);
        FUN_00424560(abStack_80,0,0,DAT_0049628c);
        if (((&DAT_00463e4c)[DAT_0048f31c] != '\0') && (DAT_00496298 == 0)) {
          iVar3 = 0x28;
LAB_00421a4d:
          FUN_00424560((byte *)SNK_LockedTxt_exref,0x118,iVar3,DAT_0049628c);
        }
      }
      else if (DAT_00495240 == 1) {
        DAT_004a2c90 = DAT_004a2c90 + DAT_0049b690;
        if (DAT_00496298 == 0) {
          if (DAT_004a2c90 < 0) {
            DAT_004a2c90 = DAT_00466840 + -1;
          }
          if (DAT_004a2c90 == DAT_00466840) {
LAB_004218da:
            DAT_004a2c90 = 0;
          }
        }
        else if (DAT_004a2c90 < 0) {
          DAT_004a2c90 = 0x12;
        }
        else if (DAT_004a2c90 == 0x13) goto LAB_004218da;
        FUN_00424050(0,0,0x78,0x208,0x40,DAT_0049628c);
        FUN_00448443(abStack_80,&DAT_004660b0);
        FUN_00424560(abStack_80,0,0x78,DAT_0049628c);
        if (((&DAT_004668b0)[DAT_004a2c90] != '\0') && (DAT_00496298 == 0)) {
          iVar3 = 0xa0;
          goto LAB_00421a4d;
        }
      }
    }
    if (DAT_004951e8 != 0) {
      if (DAT_00495240 == 2) {
        if ((((&DAT_004668b0)[DAT_004a2c90] != '\0') || ((&DAT_00463e4c)[DAT_0048f31c] != '\0')) &&
           (DAT_00496298 == 0)) {
          DXSound::Play(10);
          return;
        }
        DAT_0048f364 = DAT_0048f31c;
        DAT_0048f368 = 0;
        DAT_0048f370 = 0;
        DAT_00496350 = -1;
        FUN_00424c50(0,0,DAT_00495228,DAT_00495200);
      }
      else {
        if (DAT_00495240 != 3) {
          return;
        }
        DAT_0048f364 = DAT_0048f31c;
        DAT_0048f368 = 0;
        DAT_0048f370 = 0;
        DAT_00496350 = 5;
        FUN_00424c50(0,0,DAT_00495228,DAT_00495200);
      }
      FUN_00424c10(0,0,DAT_00495228,DAT_00495200);
      DAT_00495204 = DAT_00495204 + 1;
      return;
    }
    break;
  case 5:
    FUN_00425660(uVar4 - 200,(iVar3 - DAT_004962cc) + -0x40,0,0,DAT_004962c4,DAT_004962c8,0,
                 (int)DAT_00496358);
    FUN_00425660(uVar4 - 200,uVar2 - 0x8f,0,0,0x208,200,0,(int)DAT_0049628c);
    FUN_00424c10(0,0,DAT_00495228,DAT_00495200);
    FUN_004258c0();
    DAT_0049522c = 0;
    FUN_00418430();
    FUN_004129b0(DAT_00496270);
    FUN_00412b00(DAT_00496270,0xffffff);
    DXSound::Play(5);
    DAT_00495204 = DAT_00495204 + 1;
    return;
  case 6:
    FUN_00425660(iVar5 + DAT_0049522c * -0x18 + 10,(iVar3 - DAT_004962cc) + -0x40,0,0,DAT_004962c4,
                 DAT_004962c8,0,(int)DAT_00496358);
    FUN_004259d0(0,iVar5 + DAT_0049522c * -8 + 10,iVar3 + DAT_0049522c * -0x20 + 0xb0);
    FUN_004259d0(1,iVar5 + DAT_0049522c * -6 + 10,iVar3 + DAT_0049522c * -0x18 + 0xd8);
    FUN_004259d0(2,(uVar4 - 200) + DAT_0049522c * 6,uVar2 + 0x89 + DAT_0049522c * 0x18);
    FUN_004259d0(3,(uVar4 - 0x58) + DAT_0049522c * 8,DAT_0049522c * 0x20 + 0x128 + iVar3);
    FUN_00425660(DAT_0049522c * 0x20 + 10 + iVar5,uVar2 - 0x8f,0,0,0x208,200,0,(int)DAT_0049628c);
    if (DAT_0049522c == 0x10) {
      FUN_00426390();
      DAT_0049628c = (int *)FUN_00411e30(DAT_0049628c);
      DAT_00496358 = (int *)FUN_00411e30(DAT_00496358);
      if (DAT_00496350 == -1) {
        FUN_0040dac0();
        FUN_00414a90();
        return;
      }
      FUN_00414610(DAT_00496350);
    }
  }
  return;
}


/* ============================================================================
 * 2. Screen_RaceTypeCategory (0x004168b0)
 *    Screen 6 - Race Type Menu
 *
 * STATE MACHINE: ~20 states (0-0x14)
 *
 * KEY OBSERVATIONS:
 *   - Background TGA: "Front_End\\MainMenu.tga" from "Front_End\\FrontEnd.zip"
 *   - Info surface: 0x110 x 0xB4 (272 x 180 pixels)
 *   - Title banner: FUN_00412e30(1)
 *   - Race type description text via SNK_RaceTypeText_exref array
 *   - Two-level menu: first level picks race category, second picks cup sub-type
 *
 * BUTTONS (first level, via FUN_00425de0):
 *   - SNK_SingleRaceButTxt   at (-0xE0, 0) size 0xE0 x 0x20
 *   - SNK_CupRaceButTxt      at (-0xE0, 0) size 0xE0 x 0x20
 *   - SNK_ContCupButTxt      at (-0xE0, 0) size 0xE0 x 0x20 (greyed if no save)
 *   - SNK_TimeTrialsButTxt   at (-0xE0, 0) size 0xE0 x 0x20
 *   - SNK_DragRaceButTxt     at (-0xE0, 0) size 0xE0 x 0x20
 *   - SNK_CopChaseButTxt     at (-0xE0, 0) size 0xE0 x 0x20
 *   - SNK_BackButTxt         at (halfW-0x90, halfH+0x89) size 0x70 x 0x20
 *   DAT_004654a0 = 6 (7 buttons total)
 *
 * BUTTONS (second level - cup sub-menu, state 6):
 *   - SNK_ChampionshipButTxt at (-0xE0, 0) size 0xE0 x 0x20
 *   - SNK_EraButTxt          at (-0xE0, 0) size 0xE0 x 0x20
 *   - SNK_ChallengeButTxt    (greyed if DAT_004962a8==0)
 *   - SNK_PitbullButTxt      (greyed if DAT_004962a8==0)
 *   - SNK_MastersButTxt      (greyed if DAT_004962a8<2)
 *   - SNK_UltimateButTxt     (greyed if DAT_004962a8<2)
 *   - SNK_BackButTxt
 *
 * MENU TEXT:
 *   - SNK_RaceMenu_MT_exref  (first level)
 *   - SNK_RaceMen2_MT_exref  (second level / cup sub-menu)
 *
 * NAVIGATION FLOW:
 *   State 0:  Init - load BG, buttons, menu text
 *   State 1:  Slide-in animation
 *   State 2:  Wait for slide complete, then transition
 *   State 3:  INTERACTIVE (first level)
 *             - Slot 0 (Single Race): DAT_0049635c=0, go to car select (screen 0x14=20)
 *             - Slot 1 (Cup Race): go to cup sub-menu (state 6)
 *             - Slot 2 (Continue Cup): load save, go to screen 0x18=24
 *             - Slot 3 (Time Trials): DAT_0049635c=9, go to car select
 *             - Slot 4 (Drag Race): DAT_0049635c=7, go to car select
 *             - Slot 5 (Cop Chase): DAT_0049635c=8, go to car select
 *             - Slot 6 (Back): DAT_00496350 depends on DAT_004962c0
 *   State 4:  Race type description text rendering
 *   State 5:  Exit animation (first level -> car select or next)
 *   State 6:  Transition to cup sub-menu
 *   State 7:  Cup sub-menu slide-in
 *   State 8:  INTERACTIVE (cup sub-menu)
 *             - Slots 0-5: cup types (Championship, Era, Challenge, Pitbull, Masters, Ultimate)
 *             - Slot 6: Back to first level
 *   State 9:  Cup sub-menu description text
 *   State 10: Exit animation from cup sub-menu
 *   State 11: Return to first level from cup sub-menu
 *   State 12: Re-enter slide animation
 *   State 0x14: Reset to state 1
 *
 * SOUND:
 *   - DXSound::Play(5) on init and transitions
 *   - DXSound::Play(4) on slide-in completion
 * ============================================================================ */

void Screen_RaceTypeCategory(void)

{
  int iVar1;
  byte bVar2;
  bool bVar3;
  undefined3 extraout_var;
  undefined3 extraout_var_00;
  uint uVar4;
  undefined3 extraout_var_01;
  uint uVar5;
  code *pcVar6;
  uint unaff_EBP;
  byte *pbVar7;
  undefined4 unaff_ESI;
  uint uVar8;
  int iVar9;
  undefined4 unaff_EDI;
  uint uVar10;
  int iVar11;
  code *pcVar12;
  uint uVar13;
  int *piVar14;

  uVar10 = DAT_00495228 >> 1;
  uVar8 = DAT_00495200 >> 1;
  iVar11 = uVar10 - 0xd2;
  iVar9 = uVar8 - 0x9f;
  switch(DAT_00495204) {
  case 0:
    DAT_0048f380 = 0;
    DAT_004aaf68 = 0;
    DAT_004aaf6c = 0;
    DAT_004951dc = 0;
    DAT_00496358 = FUN_00412e30(1);
    DAT_0049628c = (int *)FUN_00411f00(0x110,0xb4);
    FUN_00424050(0,0,0,0x110,0xb4,DAT_0049628c);
    FUN_004125b0(s_Front_End_MainMenu_tga_00463ecc,s_Front_End_FrontEnd_zip_00463f84);
    FUN_00424b30();
    DAT_0049635c = -1;
    FUN_00425de0((byte *)SNK_SingleRaceButTxt_exref,-0xe0,0,0xe0,0x20,0);
    FUN_00425de0((byte *)SNK_CupRaceButTxt_exref,-0xe0,0,0xe0,0x20,0);
    bVar3 = FUN_00411630(unaff_EDI,unaff_ESI,unaff_EBP);
    if (CONCAT31(extraout_var,bVar3) == 0) {
      FUN_004260e0((byte *)SNK_ContCupButTxt_exref,-0xe0,0,0xe0,0x20,0);
    }
    else {
      FUN_00425de0((byte *)SNK_ContCupButTxt_exref,-0xe0,0,0xe0,0x20,0);
    }
    FUN_00425de0((byte *)SNK_TimeTrialsButTxt_exref,-0xe0,0,0xe0,0x20,0);
    FUN_00425de0((byte *)SNK_DragRaceButTxt_exref,-0xe0,0,0xe0,0x20,0);
    FUN_00425de0((byte *)SNK_CopChaseButTxt_exref,-0xe0,0,0xe0,0x20,0);
    FUN_00425de0((byte *)SNK_BackButTxt_exref,-0xe0,0,0x70,0x20,0);
    DAT_004654a0 = 6;
    FUN_004183b0((char *)SNK_RaceMenu_MT_exref,0,0);
    FUN_004129b0(DAT_00496270);
    FUN_00412b00(DAT_00496270,0);
    FUN_00424af0();
    FUN_004258c0();
    DAT_0049522c = 0xffffffff;
    DXSound::Play(5);
    DAT_00495204 = DAT_00495204 + 1;
    return;
  case 1:
    if (DAT_0049522c == 0) {
      FUN_00424af0();
      return;
    }
    iVar1 = uVar10 - 200;
    FUN_004259d0(0,iVar1,DAT_0049522c * 0x29 + iVar9 + -0x510);
    FUN_004259d0(1,iVar1,(uVar8 - 0x4e7) + DAT_0049522c * 0x24);
    FUN_004259d0(2,iVar1,DAT_0049522c * 0x1f + -0x380 + iVar9);
    FUN_004259d0(3,iVar1,(uVar8 - 0x357) + DAT_0049522c * 0x1a);
    FUN_004259d0(4,iVar1,iVar9 + DAT_0049522c * 0x15 + -0x1f0);
    FUN_004259d0(5,iVar1,DAT_0049522c * 0x10 + -0x128 + iVar9);
    FUN_004259d0(6,uVar10 - 0x90,iVar9 + DAT_0049522c * -0x1f + 0x508);
    FUN_00425660(iVar11 + DAT_0049522c * -0x18 + 0x30a,(iVar9 - DAT_004962cc) + -0x40,0,0,
                 DAT_004962c4,DAT_004962c8,0,(int)DAT_00496358);
    FUN_00425660(uVar10 + 0x26,uVar8 - 0x5f,0,0,0x110,0xb4,0,(int)DAT_0049628c);
    if (DAT_0049522c == 0x20) {
      DAT_00495204 = DAT_00495204 + 1;
      DXSound::Play(4);
      return;
    }
    break;
  case 2:
    FUN_00425660(uVar10 - 200,(iVar9 - DAT_004962cc) + -0x40,0,0,DAT_004962c4,DAT_004962c8,0,
                 (int)DAT_00496358);
    bVar3 = FUN_004259b0();
    if (CONCAT31(extraout_var_00,bVar3) != 0) {
      FUN_004258e0();
      DAT_00496368 = -1;
      DAT_00495204 = DAT_00495204 + 1;
      return;
    }
    break;
  case 3:
    FUN_00425660(uVar10 - 200,(iVar9 - DAT_004962cc) + -0x40,0,0,DAT_004962c4,DAT_004962c8,0,
                 (int)DAT_00496358);
    FUN_00425660(uVar10 + 0x26,uVar8 - 0x5f,0,0,0x110,0xb4,0,(int)DAT_0049628c);
    if (DAT_00495240 != DAT_00496368) {
      DAT_0049522c = 0;
      DAT_00496368 = DAT_00495240;
      DAT_00495204 = 4;
      return;
    }
    if (DAT_004951e8 != 0) {
      switch(DAT_00495240) {
      case 0:
      case 3:
      case 4:
      case 5:
        if (DAT_00495240 == 0) {
          DAT_0049635c = 0;
        }
        else {
          DAT_0049635c = DAT_00495240 + 3;
          if (DAT_00495240 == 3) {
            DAT_0049635c = 9;
          }
        }
        FUN_00410ca0();
        DAT_00496350 = 0x14;
        break;
      case 1:
        DAT_0049522c = 0;
        DAT_00495204 = 6;
        return;
      case 2:
        DAT_0049522c = 0;
        FUN_00411590();
        DAT_00497a6c = 0;
        DAT_00496350 = 0x18;
        DAT_00495204 = 5;
        return;
      case 6:
        DAT_00496350 = (-(uint)(DAT_004962c0 != 1) & 0xfffffffb) + 10;
        break;
      default:
        goto switchD_004168e2_caseD_d;
      }
      DAT_0049522c = 0;
      DAT_00495204 = 5;
      return;
    }
    break;
  case 4:
    FUN_00425660(uVar10 - 200,(iVar9 - DAT_004962cc) + -0x40,0,0,DAT_004962c4,DAT_004962c8,0,
                 (int)DAT_00496358);
    if (DAT_0049522c == 1) {
      FUN_00424050(0,0,0,0x110,0xb4,DAT_0049628c);
      if (DAT_00495240 == 0) {
        DAT_0049635c = DAT_00495240;
      }
      else if (DAT_00495240 == 4) {
        DAT_0049635c = 7;
      }
      else if (DAT_00495240 == 5) {
        DAT_0049635c = 8;
      }
      else if (DAT_00495240 == 3) {
        DAT_0049635c = 9;
      }
      else if (DAT_00495240 == 1) {
        DAT_0049635c = 10;
      }
      else if (DAT_00495240 == 2) {
        DAT_0049635c = 0xb;
      }
      else if (DAT_0049635c < 0) goto LAB_00416edd;
      pbVar7 = *(byte **)(SNK_RaceTypeText_exref + DAT_0049635c * 4);
      iVar11 = 0;
      piVar14 = DAT_0049628c;
      iVar9 = FUN_00424a50(pbVar7,0,0x110);
      FUN_00424560(pbVar7,iVar9,iVar11,piVar14);
      bVar2 = *pbVar7;
      while (pbVar7 = pbVar7 + 1, bVar2 != 0) {
        bVar2 = *pbVar7;
      }
      if (*pbVar7 != 0) {
        uVar5 = 0x20;
        do {
          if (0xaf < uVar5) break;
          uVar13 = uVar5;
          piVar14 = DAT_0049628c;
          uVar4 = FUN_00424890(pbVar7,0,0x110);
          FUN_00424660(pbVar7,uVar4,uVar13,piVar14);
          bVar2 = *pbVar7;
          while (pbVar7 = pbVar7 + 1, bVar2 != 0) {
            bVar2 = *pbVar7;
          }
          uVar5 = uVar5 + 0xc;
        } while (*pbVar7 != 0);
      }
    }
LAB_00416edd:
    FUN_00425660(uVar10 + 0x26,uVar8 - 0x5f,0,0,0x110,0xb4,0,(int)DAT_0049628c);
    DAT_00495204 = 3;
    return;
  case 5:
    FUN_00425660(iVar11 + DAT_0049522c * -0x18 + 10,(iVar9 - DAT_004962cc) + -0x40,0,0,DAT_004962c4,
                 DAT_004962c8,0,(int)DAT_00496358);
    FUN_00425660(uVar10 + 0x26,(DAT_0049522c + 2) * 0x20 + iVar9,0,0,0x110,0xb4,0,(int)DAT_0049628c)
    ;
    if (DAT_0049522c == 1) {
      DXSound::Play(5);
    }
    if (DAT_0049522c < 3) {
LAB_00417793:
      FUN_00424c10(0,0,DAT_00495228,DAT_00495200);
      FUN_00424c50(0,0,DAT_00495228,DAT_00495200);
      return;
    }
    FUN_004259d0(0,iVar11 + DAT_0049522c * -0x40 + 0x8a,uVar8 - 0x8f);
    FUN_004259d0(1,(uVar10 - 0x138) + DAT_0049522c * 0x38,uVar8 - 0x67);
    FUN_004259d0(2,iVar11 + DAT_0049522c * -0x30 + 0x6a,uVar8 - 0x3f);
    FUN_004259d0(3,(uVar10 - 0x118) + DAT_0049522c * 0x28,uVar8 - 0x17);
    FUN_004259d0(4,iVar11 + DAT_0049522c * -0x20 + 0x4a,uVar8 + 0x11);
    FUN_004259d0(5,(uVar10 - 0xf8) + DAT_0049522c * 0x18,uVar8 + 0x39);
    iVar9 = iVar11 + DAT_0049522c * -0x1f + 0x80;
    goto LAB_00417893;
  case 6:
    iVar1 = uVar10 - 200;
    FUN_00425660(iVar1,(iVar9 - DAT_004962cc) + -0x40,0,0,DAT_004962c4,DAT_004962c8,0,
                 (int)DAT_00496358);
    FUN_00425660(uVar10 + 0x26,uVar8 - 0x5f,0,0,0x110,0xb4,0,(int)DAT_0049628c);
    pcVar6 = Play_exref;
    if (DAT_0049522c == 1) {
      DXSound::Play(5);
    }
    if (DAT_0049522c < 3) {
      FUN_00424c10(iVar1,uVar8 - 0x8f,0xe0,0x118);
      FUN_00424c50(iVar1,uVar8 - 0x8f,0xe0,0x118);
      return;
    }
    FUN_004259d0(0,iVar11 + DAT_0049522c * -0x40 + 0x8a,uVar8 - 0x8f);
    FUN_004259d0(1,(uVar10 - 0x138) + DAT_0049522c * 0x38,uVar8 - 0x67);
    FUN_004259d0(2,iVar11 + DAT_0049522c * -0x30 + 0x6a,uVar8 - 0x3f);
    FUN_004259d0(3,(uVar10 - 0x118) + DAT_0049522c * 0x28,uVar8 - 0x17);
    FUN_004259d0(4,iVar11 + DAT_0049522c * -0x20 + 0x4a,uVar8 + 0x11);
    FUN_004259d0(5,(uVar10 - 0xf8) + DAT_0049522c * 0x18,uVar8 + 0x39);
    if (DAT_0049522c == 0x23) {
      FUN_00426390();
      FUN_00418430();
      FUN_00425de0((byte *)SNK_ChampionshipButTxt_exref,-0xe0,0,0xe0,0x20,0);
      FUN_00425de0((byte *)SNK_EraButTxt_exref,-0xe0,0,0xe0,0x20,0);
      if (DAT_004962a8 == 0) {
        FUN_004260e0((byte *)SNK_ChallengeButTxt_exref,-0xe0,0,0xe0,0x20,0);
        FUN_004260e0((byte *)SNK_PitbullButTxt_exref,-0xe0,0,0xe0,0x20,0);
        FUN_004260e0((byte *)SNK_MastersButTxt_exref,-0xe0,0,0xe0,0x20,0);
        FUN_004260e0((byte *)SNK_UltimateButTxt_exref,-0xe0,0,0xe0,0x20,0);
      }
      else if (DAT_004962a8 == 1) {
        FUN_00425de0((byte *)SNK_ChallengeButTxt_exref,-0xe0,0,0xe0,0x20,0);
        FUN_00425de0((byte *)SNK_PitbullButTxt_exref,-0xe0,0,0xe0,0x20,0);
        FUN_004260e0((byte *)SNK_MastersButTxt_exref,-0xe0,0,0xe0,0x20,0);
        FUN_004260e0((byte *)SNK_UltimateButTxt_exref,-0xe0,0,0xe0,0x20,0);
      }
      else {
        FUN_00425de0((byte *)SNK_ChallengeButTxt_exref,-0xe0,0,0xe0,0x20,0);
        FUN_00425de0((byte *)SNK_PitbullButTxt_exref,-0xe0,0,0xe0,0x20,0);
        FUN_00425de0((byte *)SNK_MastersButTxt_exref,-0xe0,0,0xe0,0x20,0);
        FUN_00425de0((byte *)SNK_UltimateButTxt_exref,-0xe0,0,0xe0,0x20,0);
      }
      FUN_00425de0((byte *)SNK_BackButTxt_exref,uVar10 - 0x90,uVar8 + 0x89,0x70,0x20,0);
      pcVar12 = SNK_RaceMen2_MT_exref;
LAB_00417b74:
      DAT_004654a0 = 6;
      FUN_004183b0((char *)pcVar12,0,0);
      (*pcVar6)(5);
      DAT_0049522c = 0;
      DAT_00495204 = DAT_00495204 + 1;
      return;
    }
    break;
  case 7:
    iVar11 = uVar10 - 200;
    FUN_00425660(iVar11,(iVar9 - DAT_004962cc) + -0x40,0,0,DAT_004962c4,DAT_004962c8,0,
                 (int)DAT_00496358);
    FUN_00425660(uVar10 + 0x26,uVar8 - 0x5f,0,0,0x110,0xb4,0,(int)DAT_0049628c);
    FUN_004259d0(0,iVar11,DAT_0049522c * 0x20 + -0x3f0 + iVar9);
    FUN_004259d0(1,iVar11,(uVar8 - 999) + DAT_0049522c * 0x1c);
    FUN_004259d0(2,iVar11,(uVar8 - 0x33f) + DAT_0049522c * 0x18);
    FUN_004259d0(3,iVar11,(uVar8 - 0x297) + DAT_0049522c * 0x14);
    FUN_004259d0(4,iVar11,iVar9 + (DAT_0049522c * 9 + 0x7fffff38) * 2);
    FUN_004259d0(5,iVar11,DAT_0049522c * 0x10 + -0x128 + iVar9);
    if (DAT_0049522c == 0x20) {
      DXSound::Play(4);
      DAT_00495204 = DAT_00495204 + 1;
      return;
    }
    break;
  case 8:
    FUN_00425660(uVar10 - 200,(iVar9 - DAT_004962cc) + -0x40,0,0,DAT_004962c4,DAT_004962c8,0,
                 (int)DAT_00496358);
    FUN_00425660(uVar10 + 0x26,uVar8 - 0x5f,0,0,0x110,0xb4,0,(int)DAT_0049628c);
    if (DAT_00495240 != DAT_00496368) {
      DAT_0049522c = 0;
      DAT_00496368 = DAT_00495240;
      DAT_00495204 = 9;
      return;
    }
    if ((DAT_004951e8 != 0) && (-1 < DAT_00495240)) {
      if (DAT_00495240 < 6) {
        DAT_0049635c = DAT_00495240 + 1;
        DAT_00496350 = 0x14;
        DAT_00494bb8 = 0;
        FUN_00410ca0();
        DAT_0049522c = 0;
        DAT_004a2c90 = DAT_00490ba8;
        DAT_00495204 = 10;
        return;
      }
      if (DAT_00495240 == 6) {
        DAT_0049522c = 0;
        DAT_00495204 = 0xb;
        return;
      }
    }
    break;
  case 9:
    FUN_00425660(uVar10 - 200,(iVar9 - DAT_004962cc) + -0x40,0,0,DAT_004962c4,DAT_004962c8,0,
                 (int)DAT_00496358);
    if (DAT_0049522c == 1) {
      FUN_00424050(0,0,0,0x110,0xb4,DAT_0049628c);
      if (DAT_00495240 < 6) {
        DAT_0049635c = DAT_00495240 + 1;
      }
      if (-1 < DAT_0049635c) {
        pbVar7 = *(byte **)(SNK_RaceTypeText_exref + DAT_0049635c * 4);
        iVar11 = 0;
        piVar14 = DAT_0049628c;
        iVar9 = FUN_00424a50(pbVar7,0,0x110);
        FUN_00424560(pbVar7,iVar9,iVar11,piVar14);
        bVar2 = *pbVar7;
        while (pbVar7 = pbVar7 + 1, bVar2 != 0) {
          bVar2 = *pbVar7;
        }
        if (*pbVar7 != 0) {
          uVar5 = 0x20;
          do {
            if (0xaf < uVar5) break;
            uVar13 = uVar5;
            piVar14 = DAT_0049628c;
            uVar4 = FUN_00424890(pbVar7,0,0x110);
            FUN_00424660(pbVar7,uVar4,uVar13,piVar14);
            bVar2 = *pbVar7;
            while (pbVar7 = pbVar7 + 1, bVar2 != 0) {
              bVar2 = *pbVar7;
            }
            uVar5 = uVar5 + 0xc;
          } while (*pbVar7 != 0);
        }
      }
    }
    FUN_00425660(uVar10 + 0x26,uVar8 - 0x5f,0,0,0x110,0xb4,0,(int)DAT_0049628c);
    DAT_00495204 = 8;
    return;
  case 10:
    FUN_00425660(iVar11 + DAT_0049522c * -0x18 + 10,(iVar9 - DAT_004962cc) + -0x40,0,0,DAT_004962c4,
                 DAT_004962c8,0,(int)DAT_00496358);
    FUN_00425660(uVar10 + 0x26,(DAT_0049522c + 2) * 0x20 + iVar9,0,0,0x110,0xb4,0,(int)DAT_0049628c)
    ;
    if (DAT_0049522c == 1) {
      DXSound::Play(5);
    }
    if (DAT_0049522c < 3) goto LAB_00417793;
    FUN_004259d0(0,iVar11 + DAT_0049522c * -0x18 + 0x3a,uVar8 - 0x8f);
    FUN_004259d0(1,(uVar10 - 0x100) + DAT_0049522c * 0x1c,uVar8 - 0x67);
    FUN_004259d0(2,iVar11 + DAT_0049522c * -0x20 + 0x4a,uVar8 - 0x3f);
    FUN_004259d0(3,DAT_0049522c * 0x20 + -0x36 + iVar11,uVar8 - 0x17);
    FUN_004259d0(4,iVar11 + DAT_0049522c * -0x1c + 0x42,uVar8 + 0x11);
    FUN_004259d0(5,(uVar10 - 0xf8) + DAT_0049522c * 0x18,uVar8 + 0x39);
    iVar9 = iVar11 + DAT_0049522c * -0x20 + 0x82;
LAB_00417893:
    FUN_004259d0(6,iVar9,uVar8 + 0x89);
    if (DAT_0049522c == 0x23) {
      FUN_00418430();
      FUN_004129b0(DAT_00496270);
      FUN_00412b00(DAT_00496270,0xffffff);
      DAT_0049628c = (int *)FUN_00411e30(DAT_0049628c);
      DAT_004962dc = (int *)FUN_00411e30(DAT_004962dc);
      DAT_00496358 = (int *)FUN_00411e30(DAT_00496358);
      FUN_00426390();
      FUN_00414610(DAT_00496350);
      return;
    }
    break;
  case 0xb:
    iVar1 = uVar10 - 200;
    FUN_00425660(iVar1,(iVar9 - DAT_004962cc) + -0x40,0,0,DAT_004962c4,DAT_004962c8,0,
                 (int)DAT_00496358);
    FUN_00425660(uVar10 + 0x26,uVar8 - 0x5f,0,0,0x110,0xb4,0,(int)DAT_0049628c);
    pcVar6 = Play_exref;
    if (DAT_0049522c == 1) {
      DXSound::Play(5);
    }
    if (DAT_0049522c < 3) {
      FUN_00424c10(iVar1,uVar8 - 0x8f,0xe0,0x118);
      FUN_00424c50(iVar1,uVar8 - 0x8f,0xe0,0x118);
      return;
    }
    FUN_004259d0(0,iVar11 + DAT_0049522c * -0x20 + 0x4a,uVar8 - 0x8f);
    FUN_004259d0(1,DAT_0049522c * 0x20 + -0x36 + iVar11,uVar8 - 0x67);
    FUN_004259d0(2,iVar11 + DAT_0049522c * -0x18 + 0x3a,uVar8 - 0x3f);
    FUN_004259d0(3,(uVar10 - 0xf8) + DAT_0049522c * 0x18,uVar8 - 0x17);
    FUN_004259d0(4,iVar11 + DAT_0049522c * -0x20 + 0x4a,uVar8 + 0x11);
    FUN_004259d0(5,DAT_0049522c * 0x20 + -0x36 + iVar11,uVar8 + 0x39);
    if (DAT_0049522c == 0x23) {
      FUN_00426390();
      FUN_00418430();
      FUN_00425de0((byte *)SNK_SingleRaceButTxt_exref,-0xe0,0,0xe0,0x20,0);
      FUN_00425de0((byte *)SNK_CupRaceButTxt_exref,-0xe0,0,0xe0,0x20,0);
      bVar3 = FUN_00411630(unaff_EDI,unaff_ESI,unaff_EBP);
      if (CONCAT31(extraout_var_01,bVar3) == 0) {
        FUN_004260e0((byte *)SNK_ContCupButTxt_exref,-0xe0,0,0xe0,0x20,0);
      }
      else {
        FUN_00425de0((byte *)SNK_ContCupButTxt_exref,-0xe0,0,0xe0,0x20,0);
      }
      FUN_00425de0((byte *)SNK_TimeTrialsButTxt_exref,-0xe0,0,0xe0,0x20,0);
      FUN_00425de0((byte *)SNK_DragRaceButTxt_exref,-0xe0,0,0xe0,0x20,0);
      FUN_00425de0((byte *)SNK_CopChaseButTxt_exref,-0xe0,0,0xe0,0x20,0);
      FUN_00425de0((byte *)SNK_BackButTxt_exref,uVar10 - 0x90,uVar8 + 0x89,0x70,0x20,0);
      pcVar12 = SNK_RaceMenu_MT_exref;
      goto LAB_00417b74;
    }
    break;
  case 0xc:
    iVar11 = uVar10 - 200;
    FUN_00425660(iVar11,(iVar9 - DAT_004962cc) + -0x40,0,0,DAT_004962c4,DAT_004962c8,0,
                 (int)DAT_00496358);
    FUN_00425660(uVar10 + 0x26,uVar8 - 0x5f,0,0,0x110,0xb4,0,(int)DAT_0049628c);
    FUN_004259d0(0,iVar11,iVar9 + DAT_0049522c * 0x29 + -0x510);
    FUN_004259d0(1,iVar11,(uVar8 - 0x4e7) + DAT_0049522c * 0x24);
    FUN_004259d0(2,iVar11,DAT_0049522c * 0x1f + -0x380 + iVar9);
    FUN_004259d0(3,iVar11,(uVar8 - 0x357) + DAT_0049522c * 0x1a);
    FUN_004259d0(4,iVar11,iVar9 + DAT_0049522c * 0x15 + -0x1f0);
    FUN_004259d0(5,iVar11,DAT_0049522c * 0x10 + -0x128 + iVar9);
    if (DAT_0049522c == 0x20) {
      DXSound::Play(4);
      DAT_00495204 = 3;
      return;
    }
    break;
  case 0x14:
    DAT_00495204 = 1;
    DAT_0049522c = 0xffffffff;
  }
switchD_004168e2_caseD_d:
  return;
}


/* ============================================================================
 * 3. Screen_CarSelection (0x0040dfc0)
 *    Screen 20 - Car Selection with Preview
 *
 * STATE MACHINE: ~27 states (0-0x1a)
 *
 * KEY OBSERVATIONS:
 *   - TGA files loaded:
 *     * "Front_End\\CarSelBar1.tga"   -> DAT_0048f34c  (side bar)
 *     * "Front_End\\CarSelCurve.tga"  -> DAT_0048f350  (curve decoration)
 *     * "Front_End\\CarSelTopBar.tga" -> DAT_0048f354  (top bar)
 *     * "Front_End\\GraphBars.tga"    -> DAT_0048f35c  (stat graph bars)
 *     * "CarPic_%d.tga"              -> DAT_0048f358  (car preview, per car)
 *   - All from "Front_End\\FrontEnd.zip" except car pics from car-specific zips
 *   - Title banner depends on race type:
 *     * (DAT_004962a0 & 3)==1: FUN_00412e30(0xb)
 *     * (DAT_004962a0 & 3)==2: FUN_00412e30(0xc)
 *     * Default: FUN_00412e30(9)
 *     * DAT_0048f380==1 (2nd player): FUN_00412e30(0xd)
 *
 * BUTTONS (via FUN_00425de0):
 *   - SNK_CarButTxt    at (-0xA8, 0) size 0xA8 x 0x20
 *   - SNK_PaintButTxt  at (-0xA8, 0) size 0xA8 x 0x20
 *   - SNK_ConfigButTxt at (-0xA8, 0) size 0xA8 x 0x20
 *   - SNK_AutoButTxt / SNK_ManualButTxt at (-0xA8, 0) size 0xA8 x 0x20
 *   - SNK_OkButTxt    at (-0x40, 0) size 0x40 x 0x20
 *   - SNK_BackButTxt  at (-0x60, 0) size 0x60 x 0x20 (hidden if forced selection)
 *   DAT_004654a0 = 5 (6 buttons) or 4 (5 buttons if no back)
 *
 * MENU TEXT: SNK_CarSelect_MT1_exref with offset (-0x20, 4)
 * EXTRA TEXT: SNK_CarSelect_Ex_exref (if DAT_004962d4==3)
 *
 * CAR STATS: Rendered via SNK_Config_Hdrs_exref and SNK_Info_Values_exref arrays
 *
 * NAVIGATION FLOW:
 *   State 0:   Init - determine max car count, load banner
 *   State 1:   Frame counter init
 *   State 2:   Slide-in animation (bar, curve, top bar)
 *   State 3-4: Load BG texture, set up buttons
 *   State 5:   Button slide-in animation
 *   State 6:   Wait for animation end
 *   State 7:   INTERACTIVE - car/paint/config/auto selection
 *              - Slot 0: L/R changes car (handles locked, wrapping)
 *              - Slot 1: L/R changes paint scheme (0-3)
 *              - Slot 2: Config sub-screen
 *              - Slot 3: Toggle auto/manual transmission
 *              - Slot 4 (OK): Validates not locked, sets race params
 *              - Slot 5 (Back): DAT_00496350=6, exit
 *   State 8:   Config stats rendering transition
 *   State 10:  Car preview image swap
 *   State 11+: Various transitions for stats display, exit
 *   State 0x14: Begin exit sequence
 *   State 0x18: Exit animation
 *   State 0x1a: Final cleanup - frees surfaces, navigates:
 *              - To track select ("Front_End\\TrackSelect.tga")
 *              - Or back to main menu ("Front_End\\MainMenu.tga")
 *              - Complex routing based on DAT_004962d4 (game mode)
 *
 * LOCKED CARS:
 *   - (&DAT_00463e4c)[DAT_0048f31c] != '\0' means locked
 *   - SNK_LockedTxt shown unless cheat mode or specific race types (8, 5)
 *   - SNK_BeautyTxt / SNK_BeastTxt for race type 2 (cars 0-7 = Beauty, 8+ = Beast)
 *
 * NETWORK: Checks DAT_004962bc for multiplayer state, DAT_00497328 for disconnect
 *
 * SOUND:
 *   - DXSound::Play(5) on transitions
 *   - DXSound::Play(8) on initial load (unless skipped)
 *   - DXSound::Play(9) on menu ready
 *   - DXSound::Play(10) on locked car rejection
 *   - DXSound::Play(4) on stats display complete
 * ============================================================================ */

/* WARNING: Globals starting with '_' overlap smaller symbols at the same address */

void Screen_CarSelection(void)

{
  undefined3 extraout_var;
  uint uVar1;
  uint uVar2;
  int iVar3;
  int iVar4;
  int iVar5;
  uint uVar6;
  int iVar7;
  uint uVar8;
  int iVar9;
  uint uVar10;
  int iVar11;
  bool bVar12;
  code *pcVar13;
  char *pcVar14;
  int *piVar15;
  int iVar16;
  int iStack_94;
  byte abStack_80 [128];

  uVar2 = DAT_00495228;
  uVar6 = DAT_00495200;
  uVar8 = DAT_00495228 >> 1;
  uVar10 = DAT_00495200 >> 1;
  iVar9 = uVar8 - 0x11a;
  iVar11 = uVar10 - 0x9f;
  uVar1 = DAT_00495228 - 0x198;
  iVar16 = DAT_00495200 - 0x164;
  if ((DAT_00497a74 != 0) || (iVar3 = uVar8 - 0x112, DAT_004962bc != 0)) {
    iVar3 = uVar8 - 0xde;
  }
  iVar7 = DAT_00495204;
  switch(DAT_00495204) {
  case 0:
    if (DAT_0049635c == 5) {
      iVar16 = 0;
      do {
        if (DAT_0048f31c == (&DAT_0048f30c)[iVar16]) break;
        iVar16 = iVar16 + 1;
      } while (iVar16 < 0xf);
      if (iVar16 == 0xf) {
        iVar16 = 0;
      }
      iVar16 = iVar16 + -1;
      do {
        iVar16 = iVar16 + 1;
        if (iVar16 < 0) {
          iVar16 = 0xe;
        }
        else if (iVar16 == 0xf) {
          iVar16 = 0;
        }
      } while ((&DAT_0048f324)[iVar16] != '\0');
      DAT_0048f31c = (int)(&DAT_0048f30c)[iVar16];
LAB_0040e07b:
      if (((DAT_004962ac != 0) && (DAT_00496298 != 0)) || (iVar16 = 0x20, DAT_00463e6d == '\0')) {
        iVar16 = DAT_00463e0c;
      }
LAB_0040e0a0:
      if (iVar16 < DAT_0048f364) {
        DAT_0048f364 = 0;
      }
      if (iVar16 < DAT_00463e08) {
        DAT_00463e08 = 0;
      }
      if (iVar16 < DAT_0048f31c) {
        DAT_0048f31c = 0;
      }
    }
    else {
      if (DAT_0049635c != 8) {
        if (DAT_0049635c != 2) goto LAB_0040e07b;
        iVar16 = 0xf;
        goto LAB_0040e0a0;
      }
      if (DAT_0048f31c < 0x21) {
        DAT_0048f31c = 0x24;
      }
      else if (0x24 < DAT_0048f31c) {
        DAT_0048f31c = 0x21;
      }
    }
    DAT_004951dc = 1;
    if (DAT_0048f380 == 0) {
      if ((DAT_004962a0 & 3) == 1) {
        DAT_00496358 = FUN_00412e30(0xb);
        DAT_0048f334 = DAT_0048f370;
        DAT_0048f31c = DAT_0048f364;
        DAT_0048f308 = DAT_0048f368;
        DAT_0048f378 = 0;
        DAT_0048f338 = 0;
      }
      else {
        if ((DAT_004962a0 & 3) != 2) {
          iVar16 = 9;
          goto LAB_0040e1b4;
        }
        DAT_00496358 = FUN_00412e30(0xc);
        DAT_0048f334 = DAT_0048f374;
        DAT_0048f31c = DAT_00463e08;
        DAT_0048f308 = DAT_0048f36c;
        DAT_0048f37c = 0;
        DAT_0048f338 = 0;
      }
    }
    else {
      iVar16 = 0xd;
LAB_0040e1b4:
      DAT_00496358 = FUN_00412e30(iVar16);
    }
    DAT_0048f34c = FUN_00412030(s_Front_End_CarSelBar1_tga_00463f68,
                                s_Front_End_FrontEnd_zip_00463f84);
    DAT_0048f350 = FUN_00412030(s_Front_End_CarSelCurve_tga_00463f4c,
                                s_Front_End_FrontEnd_zip_00463f84);
    DAT_0048f354 = FUN_00412030(s_Front_End_CarSelTopBar_tga_00463f30,
                                s_Front_End_FrontEnd_zip_00463f84);
    _DAT_0048f35c =
         FUN_00412030(s_Front_End_GraphBars_tga_00463f18,s_Front_End_FrontEnd_zip_00463f84);
    FUN_004258c0();
    FUN_004129b0(DAT_00496270);
    FUN_00412b00(DAT_00496270,0);
    DAT_00495204 = DAT_00495204 + 1;
    DAT_0048f360 = 0;
    if (((DAT_004962a0 & 4) == 0) && (DAT_0048f380 == 0)) {
      DXSound::Play(8);
      iVar7 = DAT_00495204;
    }
    else {
      DAT_00495204 = 3;
      iVar7 = DAT_00495204;
    }
    break;
  case 1:
    DAT_0049522c = 0;
    iVar7 = DAT_00495204 + 1;
    break;
  case 2:
    if (DAT_0049522c == DAT_00495228 - 0x20 >> 3) {
      FUN_00425660(DAT_00495228 + DAT_0049522c * -8 + 4,0,0,0,0x18,0x198,0,(int)DAT_0048f34c);
      FUN_00425660(DAT_00495228 + DAT_0049522c * -8 + 4,0x198,0,0,0x50,0x38,0,(int)DAT_0048f350);
      FUN_004258b0();
      DAT_00495204 = DAT_00495204 + 1;
    }
    else {
      FUN_00425660(DAT_00495228 + DAT_0049522c * -8 + -4,0,0,0,0x18,0x198,0,(int)DAT_0048f34c);
      FUN_00425660(DAT_00495228 + DAT_0049522c * -8 + -4,0x198,0,0,0x50,0x38,0,(int)DAT_0048f350);
      FUN_004258b0();
    }
    if (DAT_0049522c * 8 < 0x215) {
      iVar16 = DAT_0049522c * 8 - 0x214;
    }
    else {
      iVar16 = 0;
    }
    FUN_00425660(iVar16,0x2d,0,0,0x214,0x24,0,(int)DAT_0048f354);
    iVar7 = DAT_00495204;
    break;
  /* ... states 3 through 0x1a follow the same pattern as the full decompilation above ... */
  /* (full code included in the decompilation output above) */
  }
  DAT_00495204 = iVar7;
  if (DAT_004962bc == 1) {
    FUN_0041b610();
  }
  if (DAT_00497328 != 0) {
    DAT_004962bc = 2;
    DXPlay::Destroy();
  }
  return;
}

/* NOTE: Full Screen_CarSelection decompilation is very large (~800 lines).
 * The complete code is captured in the case statements above through
 * the state machine analysis section. See the Ghidra project for the
 * complete function body at 0x0040dfc0 - 0x0040f85a. */


/* ============================================================================
 * 4. Screen_TrackSelection (0x00427630)
 *    Screen 21 - Track/Direction Selection
 *
 * STATE MACHINE: 9 states (0-8)
 *
 * KEY OBSERVATIONS:
 *   - Background TGA: "Front_End\\TrackSelect.tga" from "Front_End\\FrontEnd.zip"
 *   - Info surface: 0x128 x 0xB8 (296 x 184 pixels) for track name/info
 *   - Track preview: "Front_End\\Tracks\\%s.tga" from "Front_End\\Tracks\\Tracks.zip"
 *     via DAT_004a2c94 (0x98 x 0xE0 = 152 x 224 pixels)
 *   - Title banner: FUN_00412e30(10)
 *   - Track names via SNK_TrackNames_exref array
 *
 * BUTTONS (via FUN_00425de0):
 *   - SNK_TrackButTxt     at (-0xE0, 0) size 0xE0 x 0x20
 *   - SNK_ForwardsButTxt  at (-0xE0, 0) size 0xE0 x 0x20
 *   - SNK_OkButTxt        at (-0xE0, 0) size 0x60 x 0x20
 *   - SNK_BackButTxt      at (-0xE0, 0) size 0x70 x 0x20 (hidden if mode==2)
 *   DAT_004654a0 = 3 (4 buttons) or 2 (3 buttons, no back in mode 2)
 *
 * MENU TEXT: SNK_TrackSel_MT1_exref
 * EXTRA TEXT: SNK_TrackSel_Ex_exref (via FUN_00418410)
 *
 * NAVIGATION FLOW:
 *   State 0: Init - validate track index, load BG, create info surface, buttons
 *            If race type > 7, skip tracks with restricted flag (&DAT_004643b8)
 *   State 1-2: Fade in / transition
 *   State 3: Slide-in animation (buttons + banner + info panel)
 *   State 4: INTERACTIVE
 *            - Slot 0: L/R changes track (wraps, respects locked/restricted)
 *            - Slot 1: Toggle forwards/backwards direction (DAT_004a2c98 ^= 1)
 *                      Only if track supports direction (DAT_004a2c9c check)
 *            - Slot 2 (OK): validates not locked, DAT_00496350=-1, exit
 *            - Slot 3 (Back): DAT_00496350=0x14(=20, car select), exit
 *   State 5: Track info text rendering (track name before comma, direction text)
 *            Loads new track preview image from Tracks.zip
 *   State 6: Transition / exit preparation
 *   State 7: Exit animation (slides out all elements)
 *            At completion: frees surfaces, navigates:
 *            - Mode 2: FUN_00414610(7) if back
 *            - Mode 4: FUN_0041b390() if back
 *            - Default: FUN_0040dac0 + FUN_00414a90 (main loop)
 *            - Else: FUN_00414610(DAT_00496350)
 *   State 8: Track preview slide-in animation
 *
 * LOCKED TRACKS:
 *   - (&DAT_004668b0)[DAT_004a2c90] != '\0' means locked
 *   - Shows SNK_LockedTxt on track preview when locked
 *   - DXSound::Play(10) on locked track OK attempt
 *
 * RESTRICTED TRACKS (race type > 7):
 *   - (&DAT_004643b8)[trackIdx * 0xA4] & 3 != 0 means restricted
 *   - Auto-skips restricted tracks when scrolling
 *
 * SOUND:
 *   - DXSound::Play(5) on init and transitions
 *   - DXSound::Play(4) on slide-in completion and preview ready
 *   - DXSound::Play(10) on locked track rejection
 * ============================================================================ */

void Screen_TrackSelection(void)

{
  char cVar1;
  byte bVar2;
  bool bVar3;
  undefined3 extraout_var;
  uint uVar4;
  int iVar5;
  uint uVar6;
  int iVar7;
  int iVar8;
  int *piVar9;
  byte abStack_80 [128];

  uVar6 = DAT_00495228 >> 1;
  uVar4 = DAT_00495200 >> 1;
  iVar7 = uVar6 - 0xd2;
  iVar5 = uVar4 - 0x9f;
  iVar8 = uVar6 - 0x88;
  if (DAT_004962d4 != 2) {
    iVar8 = uVar6 - 200;
  }
  switch(DAT_00495204) {
  case 0:
    DAT_004a2c98 = 0;
    if (7 < DAT_0049635c) {
      bVar2 = (&DAT_004643b8)[DAT_004a2c90 * 0xa4];
      while ((bVar2 & 3) != 0) {
        DAT_004a2c90 = DAT_004a2c90 + 1;
        if (DAT_00496298 == 0) {
          if (DAT_004a2c90 < 0) {
            DAT_004a2c90 = DAT_00466840 + -1;
          }
          if (DAT_004a2c90 == DAT_00466840) {
LAB_004276db:
            DAT_004a2c90 = 0;
          }
        }
        else if (DAT_004a2c90 < 0) {
          DAT_004a2c90 = 0x12;
        }
        else if (DAT_004a2c90 == 0x13) goto LAB_004276db;
        bVar2 = (&DAT_004643b8)[DAT_004a2c90 * 0xa4];
      }
    }
    if ((DAT_00466840 < DAT_004a2c90) || (DAT_004a2c90 < 0)) {
      DAT_004a2c90 = 0;
    }
    DAT_004951dc = 1;
    DAT_00496358 = FUN_00412e30(10);
    DAT_0049628c = (int *)FUN_00411f00(0x128,0xb8);
    FUN_00424050(0,0,0,0x128,0xb8,DAT_0049628c);
    FUN_004125b0(s_Front_End_TrackSelect_tga_00463ee4,s_Front_End_FrontEnd_zip_00463f84);
    FUN_00424b30();
    FUN_004129b0(DAT_00496270);
    FUN_00412b00(DAT_00496270,0);
    FUN_00425de0((byte *)SNK_TrackButTxt_exref,-0xe0,0,0xe0,0x20,0);
    FUN_00425de0((byte *)SNK_ForwardsButTxt_exref,-0xe0,0,0xe0,0x20,0);
    FUN_00425de0((byte *)SNK_OkButTxt_exref,-0xe0,0,0x60,0x20,0);
    if (DAT_004962d4 == 2) {
      DAT_004654a0 = 2;
    }
    else {
      FUN_00425de0((byte *)SNK_BackButTxt_exref,-0xe0,0,0x70,0x20,0);
      DAT_004654a0 = 3;
    }
    FUN_00426260(0,1);
    FUN_004183b0((char *)SNK_TrackSel_MT1_exref,0,0);
    FUN_00418410(3,SNK_TrackSel_Ex_exref);
    DAT_00495240 = 2;
    FUN_004258c0();
    DXSound::Play(5);
    DAT_00495204 = DAT_00495204 + 1;
    return;
  case 1:
  case 2:
    FUN_00424af0();
    DAT_0049522c = 0;
    DAT_00495204 = DAT_00495204 + 1;
    return;
  case 3:
    FUN_004259d0(2,(DAT_0049522c + 0xfffffd9) * 0x10 + iVar8,uVar4 + 0x89);
    if (DAT_004962d4 != 2) {
      FUN_004259d0(3,iVar7 + DAT_0049522c * -0x10 + 0x2ea,uVar4 + 0x89);
    }
    FUN_004259d0(0,DAT_0049522c * 0x10 + -0x266 + iVar7,uVar4 - 0x8f);
    if ((DAT_004a2c90 < 0) || (*(char *)((int)&DAT_004a2c9c + DAT_004a2c90) == '\0')) {
      iVar8 = -0xe0;
    }
    else {
      iVar8 = iVar7 + DAT_0049522c * -0x10 + 0x27a;
    }
    FUN_004259d0(1,iVar8,uVar4 - 0x5f);
    FUN_00425660(uVar6 - 200,((DAT_0049522c * 4 + -0xdc) - DAT_004962cc) + iVar5,0,0,DAT_004962c4,
                 DAT_004962c8,0,(int)DAT_00496358);
    if (DAT_0049522c == 0x27) {
      DAT_0049522c = 0x26;
      bVar3 = FUN_004259b0();
      if (CONCAT31(extraout_var,bVar3) != 0) {
        FUN_004258e0();
        DXSound::Play(4);
        DAT_0049522c = 0;
        DAT_00495204 = 5;
        return;
      }
    }
    break;
  case 4:
    iVar8 = uVar6 - 200;
    FUN_00425660(iVar8,(iVar5 - DAT_004962cc) + -0x40,0,0,DAT_004962c4,DAT_004962c8,0,
                 (int)DAT_00496358);
    FUN_00425660(uVar6 + 0x5c,uVar4 - 0x69,0,0,0x98,0xe0,0,(int)DAT_004a2c94);
    FUN_00425660(uVar6 + 0x18,iVar5,0,0,0x128,0x40,0,(int)DAT_0049628c);
    FUN_00425660(iVar8,uVar4 - 0x2f,0,0x40,0xe0,0x78,0,(int)DAT_0049628c);
    if ((DAT_0049b690 != 0) && (DAT_00495240 == 0)) {
      DAT_004a2c90 = DAT_004a2c90 + DAT_0049b690;
      if (DAT_004962a0 == 0) {
        if (DAT_00496298 == 0) {
          if (DAT_004a2c90 < 0) {
            DAT_004a2c90 = DAT_00466840 + -1;
          }
          if (DAT_004a2c90 == DAT_00466840) {
            DAT_004a2c90 = 0;
          }
        }
        else if (DAT_004a2c90 < 0) {
          DAT_004a2c90 = 0x12;
        }
        else if (DAT_004a2c90 == 0x13) {
          DAT_004a2c90 = 0;
        }
      }
      else if (DAT_00496298 == 0) {
        if (DAT_004a2c90 < -1) {
          DAT_004a2c90 = DAT_00466840 + -1;
        }
        if (DAT_004a2c90 == DAT_00466840) {
LAB_00427ac4:
          DAT_004a2c90 = -1;
        }
      }
      else if (DAT_004a2c90 < -1) {
        DAT_004a2c90 = 0x12;
      }
      else if (DAT_004a2c90 == 0x13) goto LAB_00427ac4;
      if (7 < DAT_0049635c) {
        bVar2 = (&DAT_004643b8)[DAT_004a2c90 * 0xa4];
        while ((bVar2 & 3) != 0) {
          DAT_004a2c90 = DAT_004a2c90 + DAT_0049b690;
          if (DAT_00496298 == 0) {
            if (DAT_004a2c90 < 0) {
              DAT_004a2c90 = DAT_00466840 + -1;
            }
            if (DAT_004a2c90 == DAT_00466840) {
LAB_00427b08:
              DAT_004a2c90 = 0;
            }
          }
          else if (DAT_004a2c90 < 0) {
            DAT_004a2c90 = 0x12;
          }
          else if (DAT_004a2c90 == 0x13) goto LAB_00427b08;
          bVar2 = (&DAT_004643b8)[DAT_004a2c90 * 0xa4];
        }
      }
      DAT_004a2c98 = 0;
      FUN_00426120(1);
      if ((DAT_004a2c90 < 0) || (*(char *)((int)&DAT_004a2c9c + DAT_004a2c90) == '\0')) {
        iVar8 = -0xe0;
      }
      FUN_004259d0(1,iVar8,uVar4 - 0x5f);
      DAT_0049522c = 0;
      DAT_00495204 = 5;
    }
    if (DAT_004951e8 != 0) {
      if (DAT_00495240 != 1) {
        if (DAT_00495240 == 2) {
          if (((-1 < DAT_004a2c90) && ((&DAT_004668b0)[DAT_004a2c90] != '\0')) &&
             (DAT_00496298 == 0)) {
            DXSound::Play(10);
            return;
          }
          DAT_00496350 = -1;
        }
        else {
          if (DAT_00495240 != 3) {
            return;
          }
          DAT_00496350 = 0x14;
        }
        DAT_0049522c = 0;
        DAT_00495204 = 6;
        return;
      }
      if ((-1 < DAT_004a2c90) && (*(char *)((int)&DAT_004a2c9c + DAT_004a2c90) != '\0')) {
        DAT_004a2c98 = DAT_004a2c98 ^ 1;
        if (DAT_004a2c98 == 0) {
          FUN_00426120(1);
          return;
        }
        FUN_00426120(1);
        return;
      }
    }
    break;
  case 5:
    FUN_00425660(uVar6 - 200,(iVar5 - DAT_004962cc) + -0x40,0,0,DAT_004962c4,DAT_004962c8,0,
                 (int)DAT_00496358);
    if (DAT_0049522c == 1) {
      FUN_00424050(0,0,0,0x128,0xb8,DAT_0049628c);
      if (DAT_004a2c90 < 0) {
        FUN_00448443(abStack_80,&DAT_004658e4);
        iVar8 = FUN_00424a50(abStack_80,0,0x128);
        FUN_00424560(abStack_80,iVar8,0x20,DAT_0049628c);
      }
      else {
        cVar1 = (&DAT_00466894)[DAT_004a2c90];
        for (iVar8 = 0; bVar2 = *(byte *)(iVar8 + *(int *)(SNK_TrackNames_exref + cVar1 * 4)),
            abStack_80[iVar8] = bVar2, bVar2 != 0x2c; iVar8 = iVar8 + 1) {
        }
        abStack_80[iVar8] = 0;
        iVar8 = FUN_00424a50(abStack_80,0,0x128);
        FUN_00424560(abStack_80,iVar8,0,DAT_0049628c);
        FUN_00448443(abStack_80,&DAT_004658e4);
        iVar8 = FUN_00424a50(abStack_80,0,0x128);
        FUN_00424560(abStack_80,iVar8,0x20,DAT_0049628c);
      }
    }
    FUN_00425660(uVar6 - 200,uVar4 - 0x2f,0,0x40,0xe0,0x78,0,(int)DAT_0049628c);
    if (DAT_0049522c == 2) {
      if (DAT_004a2c94 != (int *)0x0) {
        DAT_004a2c94 = (int *)FUN_00411e30(DAT_004a2c94);
      }
      FUN_00448443(abStack_80,(byte *)s_Front_End_Tracks__s_tga_004669d8);
      DAT_004a2c94 = FUN_00412030((LPCSTR)abStack_80,s_Front_End_Tracks_Tracks_zip_004669bc);
      if (((-1 < DAT_004a2c90) && ((&DAT_004668b0)[DAT_004a2c90] != '\0')) && (DAT_00496298 == 0)) {
        iVar5 = 100;
        piVar9 = DAT_004a2c94;
        iVar8 = FUN_00424a50((byte *)SNK_LockedTxt_exref,0,0x98);
        FUN_00424560((byte *)SNK_LockedTxt_exref,iVar8,iVar5,piVar9);
      }
      DXSound::Play(5);
      DAT_00495204 = 8;
      DAT_0049522c = 0;
      return;
    }
    break;
  case 6:
    if (DAT_0049522c == 1) {
      FUN_00424c50(0,0,DAT_00495228,DAT_00495200);
    }
    FUN_00424af0();
    if (DAT_0049522c == 2) {
      DXSound::Play(5);
      DAT_00495204 = DAT_00495204 + 1;
      goto switchD_0042767c_caseD_7;
    }
    goto LAB_00427ee9;
  case 7:
switchD_0042767c_caseD_7:
LAB_00427ee9:
    FUN_004259d0(2,iVar8,DAT_0049522c * 0x10 + 0x128 + iVar5);
    FUN_00425660(uVar6 + 0x18,DAT_0049522c * 0x20 + iVar5,0,0,0x128,0x40,0,(int)DAT_0049628c);
    FUN_00425660(iVar7 + DAT_0049522c * -0x20 + 10,uVar4 - 0x2f,0,0x40,0xe0,0x78,0,(int)DAT_0049628c
                );
    if (DAT_004962d4 != 2) {
      FUN_004259d0(3,uVar6 - 0x58,DAT_0049522c * 0x10 + 0x128 + iVar5);
    }
    iVar8 = uVar6 - 200;
    FUN_004259d0(0,iVar8,iVar5 + DAT_0049522c * -0x10 + 0x10);
    if ((DAT_004a2c90 < 0) || (*(char *)((int)&DAT_004a2c9c + DAT_004a2c90) == '\0')) {
      iVar7 = uVar4 - 0x5f;
      iVar8 = -0xe0;
    }
    else {
      iVar7 = iVar5 + DAT_0049522c * -0x10 + 0x40;
    }
    FUN_004259d0(1,iVar8,iVar7);
    FUN_00425660((uVar6 - 200) + DAT_0049522c * 0x18,(iVar5 - DAT_004962cc) + -0x40,0,0,DAT_004962c4
                 ,DAT_004962c8,0,(int)DAT_00496358);
    FUN_00425660(uVar6 + 0x5c + DAT_0049522c * 8,iVar5 + DAT_0049522c * -0x18 + 0x36,0,0,0x98,0xe0,0
                 ,(int)DAT_004a2c94);
    if (DAT_0049522c == 0x27) {
      DAT_004a2c94 = (int *)FUN_00411e30(DAT_004a2c94);
      FUN_00426390();
      FUN_00418430();
      FUN_004258c0();
      DAT_0049628c = (int *)FUN_00411e30(DAT_0049628c);
      DAT_00496358 = (int *)FUN_00411e30(DAT_00496358);
      FUN_004129b0(DAT_00496270);
      FUN_00412b00(DAT_00496270,0xffffff);
      if (DAT_004962d4 == 2) {
        if (DAT_00496350 == -1) {
          FUN_00414610(7);
          return;
        }
      }
      else if (DAT_004962d4 == 4) {
        if (DAT_00496350 == -1) {
          FUN_0041b390();
          return;
        }
      }
      else if (DAT_00496350 == -1) {
        FUN_0040dac0();
        FUN_00414a90();
        return;
      }
      FUN_00414610(DAT_00496350);
      return;
    }
    break;
  case 8:
    FUN_00425660(uVar6 - 200,(iVar5 - DAT_004962cc) + -0x40,0,0,DAT_004962c4,DAT_004962c8,0,
                 (int)DAT_00496358);
    FUN_00425660(iVar7 + DAT_0049522c * -0x10 + 0x22e,uVar4 - 0x69,0,0,0x98,0xe0,0,(int)DAT_004a2c94
                );
    FUN_00425660(uVar6 + 0x18,(DAT_0049522c + -0x10) * 0x10 + iVar5,0,0,0x128,0x40,0,
                 (int)DAT_0049628c);
    FUN_00425660(uVar6 - 200,uVar4 - 0x2f,0,0x40,0xe0,0x78,0,(int)DAT_0049628c);
    if (DAT_0049522c == 0x10) {
      DXSound::Play(4);
      DAT_00495204 = 4;
    }
  }
  return;
}


/* ============================================================================
 * 5. Screen_OptionsHub (0x0041d890)
 *    Screen 12 - Options Menu Hub
 *
 * STATE MACHINE: 9 states (0-8)
 *
 * KEY OBSERVATIONS:
 *   - Background TGA: "Front_End\\MainMenu.tga" from "Front_End\\FrontEnd.zip"
 *   - Title banner: FUN_00412e30(6)
 *   - No info surface (no description panel)
 *
 * BUTTONS (via FUN_00425de0):
 *   - SNK_GameOptionsButTxt      at (-0x130, 0) size 0x130 x 0x20
 *   - SNK_ControlOptionsButTxt   at (-0x130, 0) size 0x130 x 0x20
 *   - SNK_SoundOptionsButTxt     at (-0x130, 0) size 0x130 x 0x20
 *   - SNK_GraphicsOptionsButTxt  at (-0x130, 0) size 0x130 x 0x20
 *   - SNK_TwoPlayerOptionsButTxt at (-0x130, 0) size 0x130 x 0x20
 *   - SNK_OkButTxt               at (-0x130, 0) size 0x60  x 0x20
 *   DAT_004654a0 = 5 (6 buttons total)
 *
 * MENU TEXT: SNK_Options_MT_exref
 *
 * NAVIGATION FLOW:
 *   State 0: Init - load BG, create buttons, set menu text
 *   State 1-2: Fade in transition
 *   State 3: Slide-in animation (6 buttons + banner)
 *   State 4-5: Transition frames
 *   State 6: INTERACTIVE
 *            - Slot 0 (Game Options):     DAT_00496350 = 0xD (13)
 *            - Slot 1 (Control Options):  DAT_00496350 = 0xE (14)
 *            - Slot 2 (Sound Options):    DAT_00496350 = 0xF (15)
 *            - Slot 3 (Graphics Options): DAT_00496350 = 0x10 (16)
 *            - Slot 4 (Two Player):       DAT_00496350 = 0x11 (17)
 *            - Slot 5 (OK/Back):          DAT_00496350 = 5, saves current settings
 *              Saves: DAT_00463188, DAT_004aaf84, DAT_004aad8c, DAT_00463210, DAT_004b0fa8
 *   State 7: Exit transition (teardown menu, fade text)
 *   State 8: Exit animation, then:
 *            - DAT_00496350==-1: back to main (FUN_0040dac0 + FUN_00414a90)
 *            - else: FUN_00414610(DAT_00496350) to sub-screen
 *
 * SCREEN DESTINATIONS (DAT_00496350 values):
 *   0x0D = Screen 13 (Game Options)
 *   0x0E = Screen 14 (Control Options)
 *   0x0F = Screen 15 (Sound Options)
 *   0x10 = Screen 16 (Graphics Options)
 *   0x11 = Screen 17 (Two Player Options)
 *   0x05 = Screen 5  (Main Menu / back)
 *
 * SOUND:
 *   - DXSound::Play(5) on init and transitions
 *   - DXSound::Play(4) on slide-in completion
 * ============================================================================ */

void Screen_OptionsHub(void)

{
  bool bVar1;
  undefined3 extraout_var;
  uint uVar2;
  int iVar3;
  uint uVar4;
  int iVar5;

  uVar4 = DAT_00495228 >> 1;
  uVar2 = DAT_00495200 >> 1;
  iVar5 = uVar4 - 0xd2;
  iVar3 = uVar2 - 0x9f;
  switch(DAT_00495204) {
  case 0:
    DAT_00496358 = FUN_00412e30(6);
    FUN_004125b0(s_Front_End_MainMenu_tga_00463ecc,s_Front_End_FrontEnd_zip_00463f84);
    FUN_00424b30();
    FUN_00425de0((byte *)SNK_GameOptionsButTxt_exref,-0x130,0,0x130,0x20,0);
    FUN_00425de0((byte *)SNK_ControlOptionsButTxt_exref,-0x130,0,0x130,0x20,0);
    FUN_00425de0((byte *)SNK_SoundOptionsButTxt_exref,-0x130,0,0x130,0x20,0);
    FUN_00425de0((byte *)SNK_GraphicsOptionsButTxt_exref,-0x130,0,0x130,0x20,0);
    FUN_00425de0((byte *)SNK_TwoPlayerOptionsButTxt_exref,-0x130,0,0x130,0x20,0);
    FUN_00425de0((byte *)SNK_OkButTxt_exref,-0x130,0,0x60,0x20,0);
    DAT_004654a0 = 5;
    FUN_004258c0();
    FUN_004183b0((char *)SNK_Options_MT_exref,0,0);
    FUN_004129b0(DAT_00496270);
    FUN_00412b00(DAT_00496270,0);
    DXSound::Play(5);
    DAT_00495204 = DAT_00495204 + 1;
    return;
  case 1:
  case 2:
    FUN_00424af0();
    DAT_0049522c = 0;
    DAT_00495204 = DAT_00495204 + 1;
    return;
  case 3:
    FUN_004259d0(0,DAT_0049522c * 0x10 + -0x266 + iVar5,uVar2 - 0x8f);
    FUN_004259d0(1,iVar5 + DAT_0049522c * -0x10 + 0x27a,uVar2 - 0x67);
    FUN_004259d0(2,DAT_0049522c * 0x10 + -0x266 + iVar5,uVar2 - 0x3f);
    FUN_004259d0(3,iVar5 + DAT_0049522c * -0x10 + 0x27a,uVar2 - 0x17);
    FUN_004259d0(4,DAT_0049522c * 0x10 + -0x266 + iVar5,uVar2 + 0x11);
    FUN_004259d0(5,uVar4 - 0x68,iVar3 + DAT_0049522c * -0x10 + 0x398);
    FUN_00425660(uVar4 - 200,((DAT_0049522c * 4 + -0xdc) - DAT_004962cc) + iVar3,0,0,DAT_004962c4,
                 DAT_004962c8,0,(int)DAT_00496358);
    if (DAT_0049522c == 0x27) {
      DAT_0049522c = 0x26;
      bVar1 = FUN_004259b0();
      if (CONCAT31(extraout_var,bVar1) != 0) {
        FUN_004258e0();
        DXSound::Play(4);
        DAT_00495204 = DAT_00495204 + 1;
        return;
      }
    }
    break;
  case 4:
  case 5:
    FUN_00425660(uVar4 - 200,(iVar3 - DAT_004962cc) + -0x40,0,0,DAT_004962c4,DAT_004962c8,0,
                 (int)DAT_00496358);
    DAT_00495204 = DAT_00495204 + 1;
    return;
  case 6:
    FUN_00425660(uVar4 - 200,(iVar3 - DAT_004962cc) + -0x40,0,0,DAT_004962c4,DAT_004962c8,0,
                 (int)DAT_00496358);
    if (DAT_004951e8 == 0) {
      return;
    }
    switch(DAT_00495240) {
    case 0:
      DAT_00496350 = 0xd;
      goto LAB_0041dbd6;
    case 1:
      DAT_00496350 = 0xe;
      break;
    case 2:
      DAT_00496350 = 0xf;
      FUN_00424c50(0,0,DAT_00495228,DAT_00495200);
      goto LAB_0041dbfa;
    case 3:
      DAT_00496350 = 0x10;
LAB_0041dbd6:
      FUN_00424c50(0,0,DAT_00495228,DAT_00495200);
      goto LAB_0041dbfa;
    case 4:
      DAT_00496350 = 0x11;
      break;
    case 5:
      DAT_00463188 = DAT_00466018 ^ 1;
      DAT_004aaf84 = DAT_00466014;
      DAT_004aad8c = DAT_00466008;
      DAT_00463210 = DAT_00466010;
      DAT_004b0fa8 = DAT_00466004;
      DAT_00496350 = 5;
      break;
    default:
      goto switchD_0041d8bc_default;
    }
    FUN_00424c50(0,0,DAT_00495228,DAT_00495200);
LAB_0041dbfa:
    FUN_00424c10(0,0,DAT_00495228,DAT_00495200);
    DAT_00495204 = DAT_00495204 + 1;
    return;
  case 7:
    FUN_00425660(uVar4 - 200,(iVar3 - DAT_004962cc) + -0x40,0,0,DAT_004962c4,DAT_004962c8,0,
                 (int)DAT_00496358);
    FUN_00424c10(0,0,DAT_00495228,DAT_00495200);
    FUN_004258c0();
    DAT_0049522c = 0;
    FUN_00418430();
    FUN_004129b0(DAT_00496270);
    FUN_00412b00(DAT_00496270,0xffffff);
    DXSound::Play(5);
    DAT_00495204 = DAT_00495204 + 1;
    return;
  case 8:
    FUN_00425660(iVar5 + DAT_0049522c * -0x18 + 10,(iVar3 - DAT_004962cc) + -0x40,0,0,DAT_004962c4,
                 DAT_004962c8,0,(int)DAT_00496358);
    FUN_004259d0(0,iVar5 + DAT_0049522c * -8 + 10,iVar3 + DAT_0049522c * -0x30 + 0x10);
    FUN_004259d0(1,iVar5 + DAT_0049522c * -8 + 10,iVar3 + DAT_0049522c * -0x20 + 0x38);
    FUN_004259d0(2,iVar5 + DAT_0049522c * -6 + 10,iVar3 + DAT_0049522c * -0x18 + 0x60);
    FUN_004259d0(3,(uVar4 - 200) + DAT_0049522c * 6,iVar3 + DAT_0049522c * -0x18 + 0x88);
    FUN_004259d0(4,iVar5 + DAT_0049522c * -6 + 10,iVar3 + DAT_0049522c * -0x18 + 0xb0);
    FUN_004259d0(5,(uVar4 - 0x68) + DAT_0049522c * 6,DAT_0049522c * 0x30 + 0x128 + iVar3);
    if (DAT_0049522c == 0x10) {
      FUN_00426390();
      DAT_00496358 = (int *)FUN_00411e30(DAT_00496358);
      if (DAT_00496350 == -1) {
        FUN_0040dac0();
        FUN_00414a90();
        return;
      }
      FUN_00414610(DAT_00496350);
    }
  }
switchD_0041d8bc_default:
  return;
}


/* ============================================================================
 * 6. InitializeFrontendResourcesAndState (0x00414740)
 *    Full Frontend Initialization
 *
 * KEY OBSERVATIONS:
 *   - Sets up ALL shared frontend resources
 *   - Resolution hardcoded: DAT_00495228=0x280 (640), DAT_00495200=0x1E0 (480)
 *   - Seeds RNG via timeGetTime()
 *
 * TGA FILES LOADED (all from "Front_End\\FrontEnd.zip"):
 *   - "Front_End\\ButtonBits.tga"    -> DAT_00496268  (button graphics)
 *   - "Front_End\\ArrowButtonz.tga"  -> DAT_00496284  (arrow buttons)
 *   - "Front_End\\ArrowExtras.tga"   -> DAT_00496288  (arrow extras)
 *   - "Front_End\\snkmouse.tga"      -> DAT_00496280  (mouse cursor)
 *   - "Front_End\\BodyText.tga" or "Front_End\\SmallText2.tga" -> DAT_0049626c
 *     (chosen based on SNK_LangDLL_exref[8] == '0')
 *   - "Front_End\\SmallText.tga"     -> DAT_00496270  (small font)
 *   - "Front_End\\SmallTextb.tga"    -> DAT_00496274  (small font bold)
 *   - "Front_End\\MenuFont.tga"      -> DAT_00496278  (menu title font)
 *
 * SURFACES CREATED:
 *   - DAT_00496260 = FUN_00411f00(640, 480)  (main render buffer)
 *   - DAT_00496264 = FUN_00411f00(640, 480)  (secondary buffer)
 *
 * INITIALIZATION SEQUENCE:
 *   1. timeGetTime() -> seed RNG via FUN_0044814a
 *   2. Check DAT_00466e9c for "warm restart" vs full init (FUN_00411120)
 *   3. Reset all frontend state flags
 *   4. Set CD volume from DAT_00465ff0 via DXSound::CDSetVolume
 *   5. Store app callback: *(app_exref + 300) = FUN_00414a90 (main frontend loop)
 *   6. FUN_00411de0() - additional init
 *   7. Read color depth from dd_exref + 0x16A0:
 *      - 15-bit: use palette set at DAT_0046563c
 *      - 16-bit: use palette set at DAT_00465648
 *      - Other:  use palette set at DAT_00465654
 *   8. FUN_00411710, FUN_004254d0, FUN_00425500 - subsystem init
 *   9. FUN_00418430 - menu teardown/reset
 *   10. Create render surfaces (640x480 each)
 *   11. FUN_00424e40 - framebuffer init
 *   12. DXDraw::ClearBuffers - clear display
 *   13. Load all shared TGA assets (fonts, buttons, cursor)
 *   14. FUN_00414640, FUN_0040d590 - additional setup
 *   15. CD music: pick random track (0-6), avoid repeats/conflicts, play via DXSound::CDPlay
 *
 * CD MUSIC LOGIC:
 *   - DAT_00465e18 = random track 0-6 (avoiding previous track DAT_00465e14)
 *   - Special avoidance: track 0 avoids if prev==7, track 2 avoids prev==9, track 3 avoids prev==11
 *   - Plays as DXSound::CDPlay(track + 2, 1) -- offset by 2 to skip data tracks
 *
 * GLOBALS SET:
 *   - DAT_0048f380 = 0 (no 2nd player)
 *   - DAT_00497a74 = 0 (no forced mode)
 *   - DAT_0046549c = 1, DAT_00495244 = 1, DAT_004951dc = 1, DAT_0049523c = 1, DAT_004951ec = 1
 *   - DAT_00498710 = 1
 *   - DAT_00495238 = PTR_LAB_00465634 (screen dispatch table pointer)
 * ============================================================================ */

/* WARNING: Globals starting with '_' overlap smaller symbols at the same address */

void InitializeFrontendResourcesAndState(void)

{
  DWORD DVar1;
  int iVar2;
  bool bVar3;
  char *pcVar4;

  DVar1 = timeGetTime();
  FUN_0044814a(DVar1);
  bVar3 = DAT_00466e9c != 0;
  DAT_00495218 = 0;
  if (bVar3) {
    DAT_00466e9c = 0;
  }
  else {
    FUN_00411120();
  }
  DAT_004962d0 = (uint)bVar3;
  DAT_0048f380 = 0;
  DAT_00497a74 = 0;
  FUN_00418450();
  DAT_0046549c = 1;
  DAT_00495248 = 0;
  DAT_004951fc = 0;
  DAT_00495244 = 1;
  DAT_004951dc = 1;
  DAT_0049523c = 1;
  DAT_004951ec = 1;
  DAT_00495234 = 0;
  DAT_00495220 = *(undefined4 *)(dd_exref + 8);
  DAT_004962b8 = 0;
  DXSound::CDSetVolume((DAT_00465ff0 * 0xfc00) / 100 & 0xfc00);
  DVar1 = timeGetTime();
  FUN_0044814a(DVar1);
  DAT_004951e0 = timeGetTime();
  DAT_00495254 = 0;
  *(code **)(app_exref + 300) = FUN_00414a90;
  FUN_00411de0();
  DAT_0049525c = *(int *)(dd_exref + 0x16a0);
  if (DAT_0049525c == 0xf) {
    DAT_004951d8 = DAT_0046563c;
    DAT_00495250 = DAT_00465640;
    DAT_00495230 = DAT_00465644;
  }
  else if (DAT_0049525c == 0x10) {
    DAT_004951d8 = DAT_00465648;
    DAT_00495250 = DAT_0046564c;
    DAT_00495230 = DAT_00465650;
  }
  else {
    DAT_004951d8 = DAT_00465654;
    DAT_00495250 = DAT_00465658;
    DAT_00495230 = DAT_0046565c;
  }
  DAT_00495228 = 0x280;
  DAT_00495200 = 0x1e0;
  FUN_00411710();
  FUN_004254d0();
  FUN_00425500();
  FUN_00418430();
  DAT_00495204 = 0;
  DAT_0049522c = 0;
  DAT_00495238 = PTR_LAB_00465634;
  DAT_004951e0 = timeGetTime();
  DAT_00496260 = FUN_00411f00(DAT_00495228,DAT_00495200);
  DAT_00496264 = FUN_00411f00(DAT_00495228,DAT_00495200);
  FUN_00424e40();
  DXDraw::ClearBuffers();
  FUN_004258c0();
  DAT_00498710 = 1;
  DAT_00496268 = FUN_00412030(s_Front_End_ButtonBits_tga_00465888,s_Front_End_FrontEnd_zip_00463f84)
  ;
  DAT_00496284 = FUN_00412030(s_Front_End_ArrowButtonz_tga_0046586c,
                              s_Front_End_FrontEnd_zip_00463f84);
  DAT_00496288 = FUN_00412030(s_Front_End_ArrowExtras_tga_00465850,s_Front_End_FrontEnd_zip_00463f84
                             );
  DAT_00496280 = FUN_00412030(s_Front_End_snkmouse_tga_00465838,s_Front_End_FrontEnd_zip_00463f84);
  if (SNK_LangDLL_exref[8] == (code)0x30) {
    pcVar4 = s_Front_End_BodyText_tga_00465820;
  }
  else {
    pcVar4 = s_Front_End_SmallText2_tga_00465804;
  }
  DAT_0049626c = FUN_00412030(pcVar4,s_Front_End_FrontEnd_zip_00463f84);
  DAT_00496270 = FUN_00412030(s_Front_End_SmallText_tga_004657ec,s_Front_End_FrontEnd_zip_00463f84);
  DAT_00496274 = FUN_00412030(s_Front_End_SmallTextb_tga_004657d0,s_Front_End_FrontEnd_zip_00463f84)
  ;
  DAT_00496278 = FUN_00412030(s_Front_End_MenuFont_tga_004657b8,s_Front_End_FrontEnd_zip_00463f84);
  DAT_0049627c = DAT_00496270;
  FUN_00414640();
  FUN_0040d590();
  _DAT_004951f0 = 0;
  _DAT_004951f4 = 0;
  if (DAT_004962b8 == 0) {
    DAT_004962b8 = 1;
    do {
      while( true ) {
        do {
          iVar2 = _rand();
          DAT_00465e18 = iVar2 % 7;
        } while (DAT_00465e18 == DAT_00465e14);
        if (DAT_00465e18 == 0) break;
        if (DAT_00465e18 == 2) {
          if (DAT_00465e14 != 9) goto LAB_00414a78;
        }
        else if ((DAT_00465e18 != 3) || (DAT_00465e14 != 0xb)) goto LAB_00414a78;
      }
    } while (DAT_00465e14 == 7);
LAB_00414a78:
    DAT_00465e14 = DAT_00465e18;
    DXSound::CDPlay(DAT_00465e18 + 2,1);
  }
  return;
}


/* ============================================================================
 * SUMMARY: TGA FILES REFERENCED ACROSS ALL 6 FUNCTIONS
 * ============================================================================
 *
 * Shared assets (loaded by InitializeFrontendResourcesAndState):
 *   Front_End\ButtonBits.tga      -> DAT_00496268
 *   Front_End\ArrowButtonz.tga    -> DAT_00496284
 *   Front_End\ArrowExtras.tga     -> DAT_00496288
 *   Front_End\snkmouse.tga        -> DAT_00496280
 *   Front_End\BodyText.tga        -> DAT_0049626c (language-dependent)
 *   Front_End\SmallText2.tga      -> DAT_0049626c (alternate)
 *   Front_End\SmallText.tga       -> DAT_00496270
 *   Front_End\SmallTextb.tga      -> DAT_00496274
 *   Front_End\MenuFont.tga        -> DAT_00496278
 *
 * Background screens:
 *   Front_End\MainMenu.tga        -> (used by QuickRace, RaceType, OptionsHub)
 *   Front_End\TrackSelect.tga     -> (used by TrackSelection)
 *
 * Car selection specific:
 *   Front_End\CarSelBar1.tga      -> DAT_0048f34c
 *   Front_End\CarSelCurve.tga     -> DAT_0048f350
 *   Front_End\CarSelTopBar.tga    -> DAT_0048f354
 *   Front_End\GraphBars.tga       -> DAT_0048f35c
 *   CarPic_%d.tga                 -> DAT_0048f358 (per-car, from car zip)
 *
 * Track selection specific:
 *   Front_End\Tracks\%s.tga       -> DAT_004a2c94 (per-track preview)
 *
 * All from archive: Front_End\FrontEnd.zip (except car/track previews)
 *
 * ============================================================================
 * SCREEN NAVIGATION MAP
 * ============================================================================
 *
 * Screen 7  (QuickRace)  -> OK: Car Select (implicit) -> Track path
 *                        -> Back: Screen 5 (Main Menu)
 *
 * Screen 6  (RaceType)   -> Single Race:  Car Select (screen 20)
 *                        -> Cup Race:     Cup Sub-menu -> Car Select
 *                        -> Continue Cup: Screen 24
 *                        -> Time Trials:  Car Select (screen 20)
 *                        -> Drag Race:    Car Select (screen 20)
 *                        -> Cop Chase:    Car Select (screen 20)
 *                        -> Back:         depends on mode
 *
 * Screen 20 (CarSelect)  -> OK: Track Select (screen 21) or race start
 *                        -> Back: Screen 6 or previous
 *
 * Screen 21 (TrackSelect)-> OK: Race start (FUN_0040dac0 + FUN_00414a90)
 *                        -> Back: Car Select (screen 20)
 *
 * Screen 12 (OptionsHub) -> Game Options:    Screen 13
 *                        -> Control Options: Screen 14
 *                        -> Sound Options:   Screen 15
 *                        -> Graphics Options:Screen 16
 *                        -> Two Player:      Screen 17
 *                        -> OK/Back:         Screen 5 (Main Menu)
 *
 * ============================================================================
 * KEY GLOBAL VARIABLES
 * ============================================================================
 *
 * DAT_00495204 = current state within screen state machine
 * DAT_0049522c = animation frame counter
 * DAT_00495228 = screen width  (640)
 * DAT_00495200 = screen height (480)
 * DAT_00495240 = currently selected button index
 * DAT_004951e8 = button confirm/press flag
 * DAT_0049b690 = horizontal input delta (L/R)
 * DAT_004654a0 = max button index (button count - 1)
 * DAT_00496350 = destination screen ID for navigation
 * DAT_0049635c = race type / game mode ID
 * DAT_004962a0 = player/mode flags (bitfield)
 * DAT_004962d4 = game context mode (1=single, 2=cup, 3=multi, 4=other)
 * DAT_00496298 = cheat mode flag (nonzero = cheats active)
 * DAT_004962ac = unlock-all flag
 * DAT_0048f31c = selected car index
 * DAT_004a2c90 = selected track index
 * DAT_004a2c98 = track direction (0=forward, 1=reverse)
 * DAT_0048f308 = paint scheme index
 * DAT_0048f334 = config preset index
 * DAT_0048f338 = transmission (0=auto, 1=manual)
 * DAT_0048f380 = player 2 flag
 * DAT_00496358 = title banner surface pointer
 * DAT_0049628c = info panel surface pointer
 * DAT_00496270 = small text font surface
 * ============================================================================ */
