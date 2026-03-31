# Rename gap region functions based on decompilation analysis
# @category TD5RE

from ghidra.program.model.symbol import SourceType

fm = currentProgram.getFunctionManager()
af = currentProgram.getAddressFactory()

renames = {
    # Gap region 1: 0x414F40 - 0x4183AF (frontend area)
    "00415370": "ScreenOkDialogHandler",
    "00415e77": "MainMenu_SetScreenState10",
    "004161b2": "MainMenu_RenderCarPreview",
    "00416a94": "MainMenu_AnimateIntro",
    "00416cca": "MainMenu_HandleButtonPress",
    "00417121": "ScreenChampionshipMenu",
    "0041754d": "ChampionshipMenu_HandleInput",
    "00417584": "ChampionshipMenu_AnimateEntry",
    "004177c7": "ChampionshipMenu_AnimateButtons",
    "004179bf": "ScreenSingleRaceMenu",
    "004182a1": "ScreenCreditsRoll",
    "00418380": "CreditsRoll_Cleanup",
    "0041838d": "CreditsRoll_Finalize",

    # Gap region 2: 0x41C320 - 0x41D840 (multiplayer area)
    "0041c320": "MultiplayerLobby_RetStub",
    "0041c330": "ScreenMultiplayerLobbyCore",
    "0041c3ed": "MultiplayerLobby_InitUI",
    "0041c564": "MultiplayerLobby_HandleChat",
    "0041c7f6": "MultiplayerLobby_UpdateButtons",
    "0041c8e9": "MultiplayerLobby_WaitForHost",
    "0041cb50": "MultiplayerLobby_RenderError",
    "0041cdfe": "MultiplayerLobby_ShowYesNo",
    "0041cf41": "MultiplayerLobby_ProcessNetEvents",
    "0041cf81": "MultiplayerLobby_CleanupDisconnect",
    "0041d00d": "MultiplayerLobby_SyncPlayers",
    "0041d0a1": "MultiplayerLobby_WaitSync",
    "0041d3d5": "MultiplayerLobby_SendPlayerData",
    "0041d50e": "MultiplayerLobby_BroadcastState",
    "0041d5e6": "MultiplayerLobby_WaitReady",
    "0041d840": "FormatDisplayModeString",

    # Gap region 3: 0x41D890 - 0x421D9F (options/screens area)
    "0041d890": "ScreenOptionsMenu",
    "0041decf": "OptionsMenu_GoToScreen",
    "0041e70b": "ScreenControlOptions",
    "0041ea4d": "ControlOptions_Cleanup",
    "0041ea61": "ScreenSoundOptions",
    "0041f413": "SoundOptions_HandleInput",
    "0041f954": "ScreenGraphicsOptions",
    "004203aa": "GraphicsOptions_HandleInput",
    "00420c2a": "ScreenTwoPlayerOptions",
    "00421397": "TwoPlayerOptions_HandleInput",
    "004213a5": "ScreenQuickRaceSetup",
    "00421ae0": "QuickRaceSetup_HandleInput",
    "00421b19": "QuickRaceSetup_Cleanup",
    "00421d63": "QuickRaceSetup_Finalize",
}

renamed = 0
errors = []

for addr_str, name in renames.items():
    addr = af.getAddress("0x" + addr_str)
    func = fm.getFunctionAt(addr)
    if func is not None:
        try:
            func.setName(name, SourceType.USER_DEFINED)
            renamed += 1
        except Exception as e:
            errors.append("%s: %s" % (addr_str, str(e)))
    else:
        errors.append("%s: no function found" % addr_str)

print("Renamed %d functions" % renamed)
if errors:
    print("Errors:")
    for e in errors:
        print("  " + e)
