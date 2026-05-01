# Jython script for Ghidra headless: decompile traffic/ambient sound functions
# Target: LoadRaceAmbientSoundBuffers (0x441C60) and UpdateVehicleAudioMix (0x440B00)

from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor
import java.io.FileWriter as FileWriter

addresses = [
    (0x441C60, "LoadRaceAmbientSoundBuffers"),
    (0x441A80, "LoadVehicleSoundBank"),
    (0x440B00, "UpdateVehicleAudioMix"),
    (0x441D50, "ReleaseRaceSoundChannels"),
]

decompiler = DecompInterface()
decompiler.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()

out_path = "C:/Users/maria/Desktop/Proyectos/TD5RE/scripts/sound_traffic_decomp.txt"
fw = FileWriter(out_path)

for addr_int, name in addresses:
    addr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(addr_int)
    func = currentProgram.getFunctionManager().getFunctionAt(addr)
    fw.write("=== %s @ 0x%X ===\n" % (name, addr_int))
    if func is None:
        fw.write("[FUNCTION NOT FOUND]\n\n")
        continue
    result = decompiler.decompileFunction(func, 120, monitor)
    if result.decompileCompleted():
        fw.write(result.getDecompiledFunction().getC())
    else:
        fw.write("[DECOMPILE FAILED: %s]\n" % result.getErrorMessage())
    fw.write("\n\n")

decompiler.dispose()
fw.close()
print("Done: sound_traffic_decomp.txt")
