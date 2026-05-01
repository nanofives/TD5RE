"""
Decompile CarSelectionScreenStateMachine (0x40DFC0) and extract ALL fill/rect calls.
Also look at FillPrimaryFrontendRect callers and what functions it calls.
"""
import sys, os, traceback
sys.path.insert(0, r'C:\Users\maria\AppData\Local\Packages\PythonSoftwareFoundation.Python.3.13_qbz5n2kfra8p0\LocalCache\local-packages\Python313\site-packages')
import pyghidra

GHIDRA_INSTALL = r"C:\Users\maria\Desktop\Proyectos\TD5RE\ghidra_12.0.3_PUBLIC"
COPY_DIR = r"C:\Users\maria\Desktop\Proyectos\TD5RE"
COPY_PROJ_NAME = "TD5_mcp"
OUT_FILE = r"C:\Users\maria\Desktop\Proyectos\TD5RE\re\tools\decomp_carsel_fills.txt"

pyghidra.start(install_dir=GHIDRA_INSTALL)

from ghidra.base.project import GhidraProject
from ghidra.app.decompiler import DecompInterface, DecompileOptions
from ghidra.util.task import ConsoleTaskMonitor
from ghidra.program.model.address import Address

results = []

def log(s):
    results.append(str(s))
    print(str(s))

try:
    proj = GhidraProject.openProject(COPY_DIR, COPY_PROJ_NAME, True)
    prog = proj.openProgram("/", "TD5_d3d.exe", True)

    decomp = DecompInterface()
    opts = DecompileOptions()
    decomp.setOptions(opts)
    decomp.openProgram(prog)
    monitor = ConsoleTaskMonitor()

    fm = prog.getFunctionManager()
    addr_factory = prog.getAddressFactory()
    sym_tbl = prog.getSymbolTable()
    listing = prog.getListing()
    ref_mgr = prog.getReferenceManager()

    def decompile_at(addr_str):
        addr = addr_factory.getAddress(addr_str)
        fn = fm.getFunctionAt(addr)
        if not fn:
            fn = fm.getFunctionContaining(addr)
        if not fn:
            log(f"  NO FUNCTION at {addr_str}")
            return None, None
        res = decomp.decompileFunction(fn, 120, monitor)
        if res.decompileCompleted():
            return fn, res.getDecompiledFunction().getC()
        else:
            log(f"  DECOMPILE FAILED: {res.getErrorMessage()}")
            return fn, None

    def find_fn_by_name(name):
        for sym in sym_tbl.getSymbols(name):
            addr = sym.getAddress()
            fn = fm.getFunctionAt(addr)
            if fn:
                return fn
        return None

    # -------------------------------------------------------------------------
    # 1. Decompile CarSelectionScreenStateMachine
    # -------------------------------------------------------------------------
    log("=" * 70)
    log("CarSelectionScreenStateMachine @ 0x40DFC0")
    log("=" * 70)
    fn, code = decompile_at("0x0040DFC0")
    if fn:
        log(f"Function: {fn.getName()} at {fn.getEntryPoint()}")
    if code:
        log(code)

    # -------------------------------------------------------------------------
    # 2. Find FillPrimaryFrontendRect and any fill-like functions
    # -------------------------------------------------------------------------
    log("\n" + "=" * 70)
    log("SEARCHING FOR FILL FUNCTIONS")
    log("=" * 70)

    fill_fn_names = [
        "FillPrimaryFrontendRect",
        "FillRect",
        "Fill16BitRect",
        "FillSurface",
        "FillFrontend",
        "FillBackground",
        "FillColorRect",
    ]

    for name in fill_fn_names:
        fn = find_fn_by_name(name)
        if fn:
            log(f"\nFOUND: {name} @ {fn.getEntryPoint()}")
            # Get callers
            entry = fn.getEntryPoint()
            refs = ref_mgr.getReferencesTo(entry)
            log(f"  Callers:")
            for ref in refs:
                from_addr = ref.getFromAddress()
                caller_fn = fm.getFunctionContaining(from_addr)
                caller_name = caller_fn.getName() if caller_fn else "???"
                log(f"    {from_addr} in {caller_name}")

    # -------------------------------------------------------------------------
    # 3. Search for "Fill" in all function names
    # -------------------------------------------------------------------------
    log("\n" + "=" * 70)
    log("ALL FUNCTIONS WITH 'fill' IN NAME (case-insensitive)")
    log("=" * 70)

    all_fns = list(fm.getFunctions(True))
    for fn in all_fns:
        name = fn.getName().lower()
        if 'fill' in name or 'rect' in name or 'blit' in name or 'copy' in name or 'clear' in name:
            log(f"  {fn.getEntryPoint()} : {fn.getName()}")

    # -------------------------------------------------------------------------
    # 4. Decompile callees of CarSelectionScreenStateMachine that look like fill
    # -------------------------------------------------------------------------
    log("\n" + "=" * 70)
    log("CALLEES OF CarSelectionScreenStateMachine")
    log("=" * 70)

    main_fn_addr = addr_factory.getAddress("0x0040DFC0")
    main_fn = fm.getFunctionAt(main_fn_addr)
    if main_fn:
        from ghidra.program.model.block import BasicBlockModel
        # Get all calls from the function body
        fn_body = main_fn.getBody()
        from ghidra.program.model.address import AddressSetView
        instr_iter = listing.getInstructions(fn_body, True)
        call_targets = set()
        for instr in instr_iter:
            mnem = instr.getMnemonicString().upper()
            if mnem == 'CALL':
                refs_from = ref_mgr.getReferencesFrom(instr.getAddress())
                for ref in refs_from:
                    if ref.getReferenceType().isCall():
                        call_targets.add(str(ref.getToAddress()))
        log(f"Call targets from CarSelectionScreenStateMachine:")
        for tgt in sorted(call_targets):
            tgt_addr = addr_factory.getAddress(tgt)
            tgt_fn = fm.getFunctionAt(tgt_addr)
            tgt_name = tgt_fn.getName() if tgt_fn else "???"
            log(f"  {tgt} : {tgt_name}")

    decomp.dispose()
    proj.close(prog)
    proj.close()

except Exception as e:
    log(f"ERROR: {traceback.format_exc()}")

output = "\n".join(results)
with open(OUT_FILE, "w", encoding="utf-8") as f:
    f.write(output)

print(f"\n[Written to {OUT_FILE}]")
