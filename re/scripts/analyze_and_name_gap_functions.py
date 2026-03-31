# Analyze gap region functions, decompile them, and name them based on string refs and patterns
# @category TD5RE

from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

prog = currentProgram
fm = prog.getFunctionManager()
af = prog.getAddressFactory()
listing = prog.getListing()
refmgr = prog.getReferenceManager()

monitor = ConsoleTaskMonitor()
decomp = DecompInterface()
decomp.openProgram(prog)

gap_regions = [
    ("0x414F40", "0x4183AF"),
    ("0x41C320", "0x41D840"),
    ("0x41D890", "0x421D9F"),
]

# Collect all functions needing names
needs_naming = []
for start_str, end_str in gap_regions:
    start = af.getAddress(start_str)
    end = af.getAddress(end_str)
    it = fm.getFunctions(start, True)
    while it.hasNext():
        f = it.next()
        ep = f.getEntryPoint()
        if ep.compareTo(end) > 0:
            break
        if f.getName().startswith("FUN_"):
            needs_naming.append(f)

results = []

# Helper to find string references in a function
def find_strings_in_function(func):
    strings = []
    body = func.getBody()
    it = listing.getInstructions(body, True)
    while it.hasNext():
        inst = it.next()
        refs = inst.getReferencesFrom()
        for ref in refs:
            toAddr = ref.getToAddress()
            data = listing.getDefinedDataAt(toAddr)
            if data is not None:
                val = data.getValue()
                if val is not None and hasattr(val, 'encode'):
                    strings.append(str(val))
                # Check for pointer to string
                dt = data.getDataType()
                if dt is not None and "string" in str(dt).lower():
                    strings.append(str(data.getValue()))
            # Also check symbol names
            sym = prog.getSymbolTable().getPrimarySymbol(toAddr)
            if sym is not None:
                sname = sym.getName()
                if "exref" in sname or "SNK_" in sname:
                    strings.append(sname)
    return strings

# Helper to find called functions
def find_callees(func):
    callees = []
    body = func.getBody()
    it = listing.getInstructions(body, True)
    while it.hasNext():
        inst = it.next()
        if inst.getMnemonicString() in ["CALL"]:
            refs = inst.getReferencesFrom()
            for ref in refs:
                if ref.getReferenceType().isCall():
                    target = fm.getFunctionAt(ref.getToAddress())
                    if target is not None:
                        callees.append(target.getName())
    return callees

for func in needs_naming:
    addr = str(func.getEntryPoint())

    # Decompile
    res = decomp.decompileFunction(func, 30, monitor)
    c_code = ""
    if res is not None and res.decompileCompleted():
        c_code = res.getDecompiledFunction().getC()

    # Find string references
    strings = find_strings_in_function(func)
    callees = find_callees(func)

    # Determine name based on analysis
    name = None

    # Check for specific string patterns
    str_lower = " ".join(strings).lower()
    callee_str = " ".join(callees).lower()

    # Naming heuristics based on string references
    if "options" in str_lower and "game" in str_lower:
        name = "ScreenGameOptions"
    elif "options" in str_lower and "control" in str_lower:
        name = "ScreenControlOptions"
    elif "options" in str_lower and "sound" in str_lower:
        name = "ScreenSoundOptions"
    elif "options" in str_lower and "graphics" in str_lower:
        name = "ScreenGraphicsOptions"
    elif "options" in str_lower and "twoplayer" in str_lower:
        name = "ScreenTwoPlayerOptions"
    elif "OptionsButTxt" in " ".join(strings):
        name = "ScreenOptionsMenu"
    elif "mainmenu" in str_lower:
        if "checkout" in str_lower or "changecar" in str_lower or "startbut" in str_lower:
            name = "ScreenMultiplayerLobby"
        else:
            name = "ScreenMainMenuSub"
    elif "lobby" in str_lower or "netgame" in str_lower:
        name = "ScreenNetworkLobby"
    elif "load" in str_lower and "save" in str_lower:
        name = "ScreenLoadSave"
    elif "exit" in str_lower and "quit" in str_lower:
        name = "ScreenExitConfirm"
    elif "championship" in str_lower:
        name = "ScreenChampionship"
    elif "singlerace" in str_lower or "single_race" in str_lower:
        name = "ScreenSingleRace"
    elif "track" in str_lower and "select" in str_lower:
        name = "ScreenTrackSelect"
    elif "car" in str_lower and "select" in str_lower:
        name = "ScreenCarSelect"
    elif "password" in str_lower:
        name = "ScreenPassword"
    elif "credits" in str_lower:
        name = "ScreenCredits"
    elif "okbuttxt" in str_lower:
        if "messagewindow" in str_lower or "messagebut" in str_lower:
            name = "ScreenMessageBox"
        elif len(c_code) < 600:
            name = "ScreenOkDialog"

    # Check code patterns
    if name is None:
        if "DAT_00495204" in c_code and "switch" in c_code:
            # It's a screen handler - try to name from callees
            if "FUN_004183b0" in c_code or "FUN_00418430" in c_code:
                # Uses marquee text function
                if "DAT_00496350" in c_code:
                    name = "ScreenSubHandler_%s" % addr
                else:
                    name = "ScreenHandler_%s" % addr
            elif "FUN_004259d0" in c_code:
                # Uses button positioning
                name = "ScreenWithButtons_%s" % addr
            elif "FUN_00425660" in c_code:
                # Uses sprite rendering
                name = "ScreenRenderer_%s" % addr
            else:
                name = "ScreenHandler_%s" % addr
        elif "FUN_004259d0" in c_code:
            name = "AnimateButtons_%s" % addr
        elif "FUN_00425660" in c_code:
            name = "RenderScreen_%s" % addr
        elif "FUN_00411e30" in c_code and len(c_code) < 300:
            name = "CleanupScreen_%s" % addr
        elif "FUN_00426390" in c_code and len(c_code) < 300:
            name = "CleanupScreenAndReset_%s" % addr
        elif "RET" in c_code and len(c_code) < 100:
            name = "StubReturn_%s" % addr
        elif len(c_code) < 80:
            name = "Stub_%s" % addr

    if name is None:
        name = "UnknownGapFunc_%s" % addr

    results.append("%s|%s|%s" % (addr, name, "|".join(strings[:5]) if strings else "no_strings"))

decomp.dispose()

# Write results before renaming
with open("C:/Users/maria/Desktop/Proyectos/TD5RE/re/scripts/gap_func_analysis.txt", "w") as f:
    f.write("Analyzed: %d\n" % len(results))
    for r in results:
        f.write(r + "\n")

print("Analyzed %d functions" % len(results))
