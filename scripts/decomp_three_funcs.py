# Jython script for Ghidra headless: decompile 4 functions and dump to file
# Functions: 0x405E80, 0x446140, 0x4461C0, 0x446030, 0x404030, 0x403A20

from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor
import java.io.FileWriter as FileWriter

addresses = [
    (0x405E80, "IntegrateVehiclePoseAndContacts"),
    (0x446030, "TransformTrackVertexByMatrix_A"),
    (0x446140, "TransformTrackVertexByMatrix_B"),
    (0x4461C0, "TransformTrackVertexByMatrix_C"),
    (0x404030, "UpdatePlayerVehicleDynamics"),
    (0x403A20, "IntegrateWheelSuspensionTravel"),
]

decompiler = DecompInterface()
decompiler.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()

out_path = "C:/Users/maria/Desktop/Proyectos/TD5RE/scripts/decomp_output.txt"
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
