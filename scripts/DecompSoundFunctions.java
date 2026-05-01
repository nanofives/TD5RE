// DecompSoundFunctions.java - Headless script to decompile TD5 sound functions
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

public class DecompSoundFunctions extends GhidraScript {

    // Target function addresses
    static final long[] TARGET_ADDRS = {
        0x440A30L,  // UpdateVehicleLoopingAudioState
        0x440AB0L,  // StartTrackedVehicleAudio
        0x440AE0L,  // StopTrackedVehicleAudio
        0x440B00L,  // UpdateVehicleAudioMix (main mixer - large)
        0x441A80L,  // LoadVehicleSoundBank
        0x441C60L,  // LoadRaceAmbientSoundBuffers
        0x441D50L,  // ReleaseRaceSoundChannels
        0x441D90L,  // PlayVehicleSoundAtPosition
        0x414640L,  // LoadFrontendSoundEffects
    };

    @Override
    public void run() throws Exception {
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        FunctionManager fm = currentProgram.getFunctionManager();

        PrintWriter out = new PrintWriter(new FileWriter("C:/Users/maria/Desktop/Proyectos/TD5RE/scripts/sound_decomp_out.txt"));

        for (long addrLong : TARGET_ADDRS) {
            Address addr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(addrLong);
            Function func = fm.getFunctionAt(addr);
            if (func == null) {
                out.println("=== NO FUNCTION AT 0x" + Long.toHexString(addrLong) + " ===\n");
                continue;
            }
            out.println("=== FUNCTION: " + func.getName() + " @ 0x" + Long.toHexString(addrLong) + " ===");
            DecompileResults res = decomp.decompileFunction(func, 120, TaskMonitor.DUMMY);
            if (res.decompileCompleted()) {
                out.println(res.getDecompiledFunction().getC());
            } else {
                out.println("[DECOMPILE FAILED: " + res.getErrorMessage() + "]");
            }
            out.println();
        }

        decomp.dispose();
        out.close();
        println("DecompSoundFunctions: output written to sound_decomp_out.txt");
    }
}
