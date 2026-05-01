"""Decompile QueueFrontendSpriteBlit and its blit-flush caller."""
import pyghidra, traceback, os

GHIDRA_INSTALL = r"C:\Users\maria\Desktop\Proyectos\TD5RE\ghidra_12.0.3_PUBLIC"
COPY_DIR = r"C:\Users\maria\Desktop\Proyectos\TD5RE\TD5_copy_tmp"
COPY_PROJ_NAME = "TD5_copy"
OUT_FILE = r"C:\Users\maria\Desktop\Proyectos\TD5RE\re\tools\decomp_spriteblit.txt"

pyghidra.start(install_dir=GHIDRA_INSTALL)
from ghidra.base.project import GhidraProject
from ghidra.app.decompiler import DecompInterface, DecompileOptions
from ghidra.util.task import ConsoleTaskMonitor

results = []
try:
    proj = GhidraProject.openProject(COPY_DIR, COPY_PROJ_NAME, True)
    prog = proj.openProgram("/", "TD5_d3d.exe", True)
    decomp = DecompInterface()
    decomp.setOptions(DecompileOptions())
    decomp.openProgram(prog)
    monitor = ConsoleTaskMonitor()
    fm = prog.getFunctionManager()
    sym_tbl = prog.getSymbolTable()

    # Find by name
    targets = ["QueueFrontendSpriteBlit", "FlushFrontendSpriteBlits",
               "ExecuteFrontendSpriteBlits", "DrawFrontendSpriteQueue",
               "PresentFrontendSprites", "BlitFrontendSprites",
               "RenderFrontendDisplayModeHighlight"]

    found = []
    for fn in fm.getFunctions(True):
        if fn.getName() in targets:
            found.append(fn)

    results.append(f"Found {len(found)} target functions:")
    for f in found:
        results.append(f"  {f.getEntryPoint()} {f.getName()}")

    for fn in sorted(found, key=lambda f: f.getEntryPoint().getOffset()):
        results.append(f"\n{'='*60}\nFUNCTION: {fn.getEntryPoint()} {fn.getName()}\n{'='*60}")
        res = decomp.decompileFunction(fn, 90, monitor)
        if res.decompileCompleted():
            results.append(res.getDecompiledFunction().getC())
        else:
            results.append(f"FAILED: {res.getErrorMessage()}")

    decomp.dispose()
    proj.close(prog)
    proj.close()
except Exception as e:
    results.append(f"ERROR: {traceback.format_exc()}")

output = "\n".join(results)
print(output[:5000])
with open(OUT_FILE, "w", encoding="utf-8") as f:
    f.write(output)
print(f"\nWritten to {OUT_FILE}")
