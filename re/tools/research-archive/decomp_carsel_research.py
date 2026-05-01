"""
Decompile CarSelectionScreenStateMachine and RaceTypeCategoryMenuStateMachine
for RE research: background TGA loading in state 0.
"""
import sys, os, traceback
sys.path.insert(0, r'C:\Users\maria\AppData\Local\Packages\PythonSoftwareFoundation.Python.3.13_qbz5n2kfra8p0\LocalCache\local-packages\Python313\site-packages')
import pyghidra

GHIDRA_INSTALL = r"C:\Users\maria\Desktop\Proyectos\TD5RE\ghidra_12.0.3_PUBLIC"
COPY_DIR = r"C:\Users\maria\Desktop\Proyectos\TD5RE"
COPY_PROJ_NAME = "TD5_research"
OUT_FILE = r"C:\Users\maria\Desktop\Proyectos\TD5RE\re\tools\decomp_carsel_research.txt"

TARGET_FUNCTIONS = [
    ("CarSelectionScreenStateMachine", "0x0040DFC0"),
    ("RaceTypeCategoryMenuStateMachine", "0x004168B0"),
]

pyghidra.start(install_dir=GHIDRA_INSTALL)

from ghidra.base.project import GhidraProject
from ghidra.app.decompiler import DecompInterface, DecompileOptions
from ghidra.util.task import ConsoleTaskMonitor

results = []

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

    for fn_name, fn_addr in TARGET_FUNCTIONS:
        results.append(f"\n{'='*70}")
        results.append(f"TARGET: {fn_name} @ {fn_addr}")
        results.append('='*70)

        # Try by name first
        fn = None
        for sym in sym_tbl.getSymbols(fn_name):
            addr = sym.getAddress()
            fn = fm.getFunctionAt(addr)
            if fn:
                results.append(f"Found by name at {fn.getEntryPoint()}")
                break

        # Fall back to address
        if not fn:
            addr = addr_factory.getAddress(fn_addr)
            fn = fm.getFunctionAt(addr)
            if fn:
                results.append(f"Found by address {fn_addr}: {fn.getName()}")

        if not fn:
            results.append(f"NOT FOUND: {fn_name} / {fn_addr}")
            continue

        res = decomp.decompileFunction(fn, 120, monitor)
        if res.decompileCompleted():
            c_code = res.getDecompiledFunction().getC()
            results.append(c_code)
        else:
            results.append(f"DECOMPILE FAILED: {res.getErrorMessage()}")

    decomp.dispose()
    proj.close(prog)
    proj.close()

except Exception as e:
    results.append(f"ERROR: {traceback.format_exc()}")

output = "\n".join(results)
with open(OUT_FILE, "w", encoding="utf-8") as f:
    f.write(output)

# Print full output
print(output)
print(f"\n[Written to {OUT_FILE}]")
