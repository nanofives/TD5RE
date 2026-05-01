# Jython script for Ghidra headless: decompile wheel/hub functions
# Functions: 0x446A70 (hub-cap template), 0x446F00 (wheel billboard), 0x442770 (LoadRaceTexturePages)

from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor
import java.io.FileWriter as FileWriter

addresses = [
    (0x446A70, "WheelBillboard_HubCapTemplate"),
    (0x446F00, "RenderVehicleWheelBillboards"),
    (0x442770, "LoadRaceTexturePages"),
    (0x446E00, "WheelBillboard_Unknown1"),
    (0x446B00, "WheelBillboard_Unknown2"),
    (0x446C00, "WheelBillboard_Unknown3"),
]

decompiler = DecompInterface()
decompiler.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()

out_path = "C:/Users/maria/Desktop/Proyectos/TD5RE/scripts/decomp_wheel_output.txt"
fw = FileWriter(out_path)

for addr_int, name in addresses:
    addr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(addr_int)
    func = currentProgram.getFunctionManager().getFunctionAt(addr)
    if func is None:
        fw.write("=== %s @ 0x%X: FUNCTION NOT FOUND ===\n\n" % (name, addr_int))
        continue
    result = decompiler.decompileFunction(func, 60, monitor)
    if result is None or result.getDecompiledFunction() is None:
        fw.write("=== %s @ 0x%X: DECOMPILE FAILED ===\n\n" % (name, addr_int))
        continue
    code = result.getDecompiledFunction().getC()
    fw.write("=== %s @ 0x%X ===\n" % (name, addr_int))
    fw.write(code)
    fw.write("\n\n")

fw.close()
print("DONE: wrote to " + out_path)
