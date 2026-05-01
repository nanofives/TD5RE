"""
Decompile CarSelectionScreenStateMachine (0x40DFC0) - check project contents first
"""
import sys, os, traceback
sys.path.insert(0, r'C:\Users\maria\AppData\Local\Packages\PythonSoftwareFoundation.Python.3.13_qbz5n2kfra8p0\LocalCache\local-packages\Python313\site-packages')
import pyghidra

GHIDRA_INSTALL = r"C:\Users\maria\Desktop\Proyectos\TD5RE\ghidra_12.0.3_PUBLIC"
COPY_DIR = r"C:\Users\maria\Desktop\Proyectos\TD5RE"
COPY_PROJ_NAME = "TD5_mcp"
OUT_FILE = r"C:\Users\maria\Desktop\Proyectos\TD5RE\re\tools\decomp_carsel_fills2.txt"

pyghidra.start(install_dir=GHIDRA_INSTALL)

from ghidra.base.project import GhidraProject
from ghidra.app.decompiler import DecompInterface, DecompileOptions
from ghidra.util.task import ConsoleTaskMonitor

results = []
def log(s):
    results.append(str(s))
    print(str(s))

try:
    proj = GhidraProject.openProject(COPY_DIR, COPY_PROJ_NAME, True)
    log(f"Project opened: {COPY_PROJ_NAME}")

    # List available programs
    files = proj.getRootFolder().getFiles()
    log(f"Programs in project:")
    for f in files:
        log(f"  {f.getName()} @ {f.getPathname()}")

    # Try to open program
    try:
        prog = proj.openProgram("/", "TD5_d3d.exe", True)
        log(f"Program opened: {prog.getName()}")
        log(f"Image base: {prog.getImageBase()}")

        fm = prog.getFunctionManager()
        fn_count = fm.getFunctionCount()
        log(f"Function count: {fn_count}")

        if fn_count > 0:
            addr_factory = prog.getAddressFactory()
            sym_tbl = prog.getSymbolTable()

            # Check if function exists at 0x40DFC0
            addr = addr_factory.getAddress("0x0040DFC0")
            fn = fm.getFunctionAt(addr)
            log(f"Function at 0x40DFC0: {fn.getName() if fn else 'NOT FOUND'}")

            # Search by name
            for sym in sym_tbl.getSymbols("CarSelectionScreenStateMachine"):
                log(f"Symbol CarSelectionScreenStateMachine: {sym.getAddress()}")

            # List first 20 functions to verify content
            log("First 20 functions:")
            fns = list(fm.getFunctions(True))
            for f in fns[:20]:
                log(f"  {f.getEntryPoint()} : {f.getName()}")

    except Exception as e2:
        log(f"Error opening program: {e2}")

    proj.close()

except Exception as e:
    log(f"ERROR: {traceback.format_exc()}")

output = "\n".join(results)
with open(OUT_FILE, "w", encoding="utf-8") as f:
    f.write(output)
print(f"\n[Written to {OUT_FILE}]")
