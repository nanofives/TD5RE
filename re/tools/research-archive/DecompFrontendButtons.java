// Decompile FUN_00424560 and FUN_00425b60 from the TD5 project
// @author td5re
// @category Analysis
// @menupath
// @toolbar

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.base.project.GhidraProject;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import java.io.File;
import java.io.PrintWriter;

public class DecompFrontendButtons extends GhidraScript {

    @Override
    public void run() throws Exception {
        GhidraProject td5proj = null;
        try {
            td5proj = GhidraProject.openProject(
                "C:/Users/maria/Desktop/Proyectos/TD5RE", "TD5", true);

            Program prog = td5proj.openProgram("/", "TD5_d3d.exe", true);

            DecompInterface decomp = new DecompInterface();
            DecompileOptions opts = new DecompileOptions();
            decomp.setOptions(opts);
            decomp.openProgram(prog);

            String[] addrs = {"00424560", "00425b60"};
            StringBuilder sb = new StringBuilder();

            for (String addrStr : addrs) {
                Address addr = prog.getAddressFactory().getAddress("0x" + addrStr);
                Function fn = prog.getFunctionManager().getFunctionAt(addr);
                sb.append("=== FUN_").append(addrStr).append(" ===\n");
                if (fn == null) {
                    sb.append("NO FUNCTION AT THIS ADDRESS\n\n");
                    continue;
                }
                sb.append("Name: ").append(fn.getName()).append("\n");
                DecompileResults res = decomp.decompileFunction(fn, 60, monitor);
                if (res.decompileCompleted()) {
                    sb.append(res.getDecompiledFunction().getC()).append("\n");
                } else {
                    sb.append("DECOMPILE FAILED: ").append(res.getErrorMessage()).append("\n");
                }
            }

            decomp.dispose();
            td5proj.close(prog);

            // Write output
            PrintWriter pw = new PrintWriter(
                new File("C:/Users/maria/Desktop/Proyectos/TD5RE/re/tools/decomp_buttons_out.txt"));
            pw.print(sb.toString());
            pw.close();

            println(sb.toString());

        } finally {
            if (td5proj != null) td5proj.close();
        }
    }
}
