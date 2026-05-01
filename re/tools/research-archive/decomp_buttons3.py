"""
Copy the TD5 project database to a temp location, then open and decompile.
This bypasses the file channel lock on the TD5 project.
"""
import pyghidra
import sys, os, traceback, shutil

GHIDRA_INSTALL = r"C:\Users\maria\Desktop\Proyectos\TD5RE\ghidra_12.0.3_PUBLIC"
ORIG_PROJ_REP = r"C:\Users\maria\Desktop\Proyectos\TD5RE\TD5.rep"
COPY_DIR = r"C:\Users\maria\Desktop\Proyectos\TD5RE\TD5_copy_tmp"
COPY_PROJ_NAME = "TD5_copy"
OUT_FILE = r"C:\Users\maria\Desktop\Proyectos\TD5RE\re\tools\decomp_buttons_out.txt"

TARGETS = ["0x00424560", "0x00425b60"]

# Copy the project rep to a new location (no .lock file goes with it)
copy_rep = os.path.join(COPY_DIR, COPY_PROJ_NAME + ".rep")
copy_gpr = os.path.join(COPY_DIR, COPY_PROJ_NAME + ".gpr")
orig_gpr = r"C:\Users\maria\Desktop\Proyectos\TD5RE\TD5.gpr"

os.makedirs(COPY_DIR, exist_ok=True)
if os.path.exists(copy_rep):
    shutil.rmtree(copy_rep)
shutil.copytree(ORIG_PROJ_REP, copy_rep)
shutil.copy2(orig_gpr, copy_gpr)

print(f"Copied project to {copy_rep}")

pyghidra.start(install_dir=GHIDRA_INSTALL)

from ghidra.base.project import GhidraProject
from ghidra.app.decompiler import DecompInterface, DecompileOptions
from ghidra.util.task import ConsoleTaskMonitor

results = []

try:
    proj = GhidraProject.openProject(COPY_DIR, COPY_PROJ_NAME, True)
    prog = proj.openProgram("/", "TD5_d3d.exe", True)
    print(f"Opened program: {prog.getName()}, functions: {prog.getFunctionManager().getFunctionCount()}")

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
