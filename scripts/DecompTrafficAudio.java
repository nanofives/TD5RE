// DecompTrafficAudio.java - Decompile traffic/ambient sound slot functions
// Run via analyzeHeadless with -postScript
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.address.Address;
import ghidra.util.task.TaskMonitor;
import java.io.PrintWriter;
import java.io.FileWriter;

public class DecompTrafficAudio extends GhidraScript {

    // Focus: ambient load + main mixer (traffic section)
    static final long[] TARGET_ADDRS = {
        0x441C60L,  // LoadRaceAmbientSoundBuffers
        0x441A80L,  // LoadVehicleSoundBank
        0x440B00L,  // UpdateVehicleAudioMix (main mixer)
        0x441D50L,  // ReleaseRaceSoundChannels
    };

    @Override
    public void run() throws Exception {
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        FunctionManager fm = currentProgram.getFunctionManager();

        PrintWriter out = new PrintWriter(new FileWriter("C:/Users/maria/Desktop/Proyectos/TD5RE/scripts/traffic_audio_decomp.txt"));

        for (long addrLong : TARGET_ADDRS) {
            Address addr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(addrLong);
            Function func = fm.getFunctionAt(addr);
            if (func == null) {
                out.println("=== NO FUNCTION AT 0x" + Long.toHexString(addrLong) + " ===\n");
                continue;
            }
            out.println("=== FUNCTION: " + func.getName() + " @ 0x" + Long.toHexString(addrLong) + " ===");
            DecompileResults res = decomp.decompileFunction(func, 180, TaskMonitor.DUMMY);
            if (res.decompileCompleted()) {
                out.println(res.getDecompiledFunction().getC());
            } else {
                out.println("[DECOMPILE FAILED: " + res.getErrorMessage() + "]");
            }
            out.println();
        }

        decomp.dispose();
        out.close();
        println("DecompTrafficAudio: output written to traffic_audio_decomp.txt");
    }
}
