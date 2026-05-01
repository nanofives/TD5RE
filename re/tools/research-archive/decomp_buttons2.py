"""
Decompile FUN_00424560 and FUN_00425b60 from the analyzed TD5 project.
Uses pyghidra open_project to open existing analyzed project.
"""
import pyghidra
import sys, os, traceback

GHIDRA_INSTALL = r"C:\Users\maria\Desktop\Proyectos\TD5RE\ghidra_12.0.3_PUBLIC"
PROJECT_LOCATION = r"C:\Users\maria\Desktop\Proyectos\TD5RE"
PROJECT_NAME = "TD5"
PROGRAM_PATH = "/TD5_d3d.exe"
OUT_FILE = r"C:\Users\maria\Desktop\Proyectos\TD5RE\re\tools\decomp_buttons_out.txt"

TARGETS = ["0x00424560", "0x00425b60"]

pyghidra.start(install_dir=GHIDRA_INSTALL)

from ghidra.base.project import GhidraProject
from ghidra.app.decompiler import DecompInterface, DecompileOptions
from ghidra.util.task import ConsoleTaskMonitor

results = []

try:
    proj = GhidraProject.openProject(PROJECT_LOCATION, PROJECT_NAME, True)
    prog = proj.openProgram("/", "TD5_d3d.exe", True)

    decomp = DecompInterface()
    opts = DecompileOptions()
    decomp.setOptions(opts)
    decomp.openProgram(prog)

    monitor = ConsoleTaskMonitor()
    fm = prog.getFunctionManager()
    addr_factory = prog.getAddressFactory()

    for addrStr in TARGETS:
        addr = addr_factory.getAddress(addrStr)
        fn = fm.getFunctionAt(addr)
        results.append(f"=== {addrStr} ===")
        if fn is None:
            results.append(f"NO FUNCTION AT THIS ADDRESS\n")
            continue
        results.append(f"Name: {fn.getName()}")
        res = decomp.decompileFunction(fn, 90, monitor)
        if res.decompileCompleted():
            results.append(res.getDecompiledFunction().getC())
        else:
            results.append(f"FAILED: {res.getErrorMessage()}")
        results.append("")

    decomp.dispose()
    proj.close(prog)
    proj.close()

except Exception as e:
    results.append(f"ERROR: {traceback.format_exc()}")

output = "\n".join(results)
print(output)
with open(OUT_FILE, "w", encoding="utf-8") as f:
    f.write(output)
print(f"\nWritten to {OUT_FILE}")
