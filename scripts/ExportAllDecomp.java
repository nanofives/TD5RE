// ExportAllDecomp.java — Headless full-project decompilation export.
//
// Freezes the annotated TD5 Ghidra project into greppable text under
// re/ghidra_export/ so sessions can answer "what did the original do?"
// without booting live Ghidra (part of the Ghidra soft-retirement plan).
//
// Output layout:
//   re/ghidra_export/functions/0x<addr>_<name>.c   one file per function
//   re/ghidra_export/symbols.csv                   all functions (addr,name,size,cc,thunk)
//   re/ghidra_export/globals.csv                   non-function primary symbols (addr,name,type)
//   re/ghidra_export/structs.h                     all user/program data types as C decls
//   re/ghidra_export/EXPORT_INFO.txt               provenance + counts
//
// Run (read-only, does not modify the project):
//   ghidra_12.0.3_PUBLIC/support/analyzeHeadless.bat . TD5 -process TD5_d3d.exe \
//     -noanalysis -readOnly -scriptPath scripts -postScript ExportAllDecomp.java
//
// Optional script arg 1 overrides the output dir.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.DataTypeManager;
import ghidra.program.model.data.DataTypeWriter;
import ghidra.program.model.listing.CodeUnit;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;
import ghidra.program.model.symbol.SymbolType;
import ghidra.util.task.TaskMonitor;
import java.io.File;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;

public class ExportAllDecomp extends GhidraScript {

    static String sanitize(String s) {
        return s.replaceAll("[^A-Za-z0-9_.\\-]", "_");
    }

    static String csv(String s) {
        if (s == null) return "";
        if (s.contains(",") || s.contains("\"") || s.contains("\n")) {
            return "\"" + s.replace("\"", "\"\"") + "\"";
        }
        return s;
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String outRoot = (args.length > 0) ? args[0]
                : "C:/Users/maria/Desktop/Proyectos/TD5RE/re/ghidra_export";

        File funcDir = new File(outRoot, "functions");
        funcDir.mkdirs();

        DecompInterface decomp = new DecompInterface();
        DecompileOptions opts = new DecompileOptions();
        opts.grabFromProgram(currentProgram);
        decomp.setOptions(opts);
        decomp.openProgram(currentProgram);

        PrintWriter symbols = new PrintWriter(new FileWriter(new File(outRoot, "symbols.csv")));
        symbols.println("address,name,size_bytes,calling_convention,is_thunk,file");

        int ok = 0, failed = 0, thunks = 0;
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Function f = it.next();
            if (f.isExternal()) continue;

            String addr = "0x" + f.getEntryPoint().toString();
            String fileName = addr + "_" + sanitize(f.getName()) + ".c";
            long size = f.getBody().getNumAddresses();

            symbols.println(addr + "," + csv(f.getName()) + "," + size + ","
                    + csv(f.getCallingConventionName()) + "," + f.isThunk() + ","
                    + "functions/" + fileName);

            PrintWriter out = new PrintWriter(new FileWriter(new File(funcDir, fileName)));
            out.println("// FUNCTION: " + f.getName() + " @ " + addr);
            out.println("// SIGNATURE: " + f.getSignature().getPrototypeString());
            out.println("// SIZE: " + size + " bytes  CC: " + f.getCallingConventionName());

            String plate = currentProgram.getListing().getComment(
                    CodeUnit.PLATE_COMMENT, f.getEntryPoint());
            if (plate != null && !plate.isEmpty()) {
                out.println("// PLATE COMMENT:");
                for (String line : plate.split("\n")) {
                    out.println("//   " + line);
                }
            }

            if (f.isThunk()) {
                Function target = f.getThunkedFunction(true);
                out.println("// THUNK -> " + (target != null
                        ? target.getName() + " @ 0x" + target.getEntryPoint() : "?"));
                out.close();
                thunks++;
                continue;
            }

            DecompileResults res = decomp.decompileFunction(f, 90, TaskMonitor.DUMMY);
            if (res != null && res.decompileCompleted()) {
                out.println();
                out.println(res.getDecompiledFunction().getC());
                ok++;
            } else {
                out.println("// [DECOMPILE FAILED: "
                        + (res != null ? res.getErrorMessage() : "null result") + "]");
                failed++;
            }
            out.close();

            if ((ok + failed + thunks) % 50 == 0) {
                println("ExportAllDecomp: " + (ok + failed + thunks) + " functions processed...");
            }
        }
        symbols.close();
        decomp.dispose();

        // Non-function primary symbols (globals, labels) for address->name lookups.
        PrintWriter globals = new PrintWriter(new FileWriter(new File(outRoot, "globals.csv")));
        globals.println("address,name,symbol_type,namespace");
        int nglobals = 0;
        SymbolIterator sit = currentProgram.getSymbolTable().getAllSymbols(false);
        while (sit.hasNext() && !monitor.isCancelled()) {
            Symbol s = sit.next();
            if (!s.isPrimary() || s.isExternal()) continue;
            SymbolType t = s.getSymbolType();
            if (t == SymbolType.FUNCTION) continue;
            if (t != SymbolType.LABEL) continue;
            globals.println("0x" + s.getAddress() + "," + csv(s.getName()) + ","
                    + t + "," + csv(s.getParentNamespace().getName()));
            nglobals++;
        }
        globals.close();

        // All program data types (structs, unions, enums, typedefs) as C declarations.
        String structsNote = "";
        try {
            DataTypeManager dtm = currentProgram.getDataTypeManager();
            List<DataType> all = new ArrayList<>();
            Iterator<DataType> dit = dtm.getAllDataTypes();
            while (dit.hasNext()) {
                all.add(dit.next());
            }
            StringWriter sw = new StringWriter();
            DataTypeWriter dtw = new DataTypeWriter(dtm, sw);
            dtw.write(all, TaskMonitor.DUMMY);
            PrintWriter structs = new PrintWriter(new FileWriter(new File(outRoot, "structs.h")));
            structs.println("// All data types from the TD5 Ghidra project (structs verified");
            structs.println("// against the 0x388 actor stride — see CLAUDE.md).");
            structs.println();
            structs.print(sw.toString());
            structs.close();
        } catch (Exception e) {
            structsNote = "structs.h export FAILED: " + e.getMessage();
            println("ExportAllDecomp: " + structsNote);
        }

        PrintWriter info = new PrintWriter(new FileWriter(new File(outRoot, "EXPORT_INFO.txt")));
        info.println("Source: Ghidra project TD5 / program " + currentProgram.getName());
        info.println("Program hash (MD5): " + currentProgram.getExecutableMD5());
        info.println("Functions decompiled OK: " + ok);
        info.println("Thunks (stub files):     " + thunks);
        info.println("Decompile failures:      " + failed);
        info.println("Global labels:           " + nglobals);
        if (!structsNote.isEmpty()) info.println("NOTE: " + structsNote);
        info.println();
        info.println("Regenerate with:");
        info.println("  ghidra_12.0.3_PUBLIC/support/analyzeHeadless.bat . TD5 -process TD5_d3d.exe \\");
        info.println("    -noanalysis -readOnly -scriptPath scripts -postScript ExportAllDecomp.java");
        info.close();

        println("ExportAllDecomp DONE: " + ok + " decompiled, " + thunks + " thunks, "
                + failed + " failed, " + nglobals + " globals -> " + outRoot);
    }
}
