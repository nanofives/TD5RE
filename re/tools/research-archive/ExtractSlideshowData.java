// Ghidra headless script - Extract frontend slideshow functions from TD5_d3d.exe
// @category TD5RE

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
import java.util.*;

public class ExtractSlideshowData extends GhidraScript {
    
    private DecompInterface decompiler;

    @Override
    public void run() throws Exception {
        println("=== TD5 Frontend Slideshow Analysis ===");
        println("Program: " + currentProgram.getName());
        
        decompiler = new DecompInterface();
        DecompileOptions opts = new DecompileOptions();
        decompiler.setOptions(opts);
        decompiler.openProgram(currentProgram);
        
        // 1. Search function names
        println("\n=== FUNCTION SEARCH (slide/background/frontend/anim/attract) ===");
        FunctionManager fm = currentProgram.getFunctionManager();
        String[] keywords = {"slide", "background", "frontend", "anim", "attract",
                             "gallery", "menu", "screen", "display", "cycle", "blit"};
        Set<String> seen = new HashSet<>();
        for (Function fn : fm.getFunctions(true)) {
            String name = fn.getName().toLowerCase();
            for (String kw : keywords) {
                if (name.contains(kw) && !seen.contains(fn.getEntryPoint().toString())) {
                    seen.add(fn.getEntryPoint().toString());
                    println("  " + fn.getEntryPoint() + "  " + fn.getName());
                    break;
                }
            }
        }
        
        // 2. Decompile key functions
        String[] targets = {
            "00415490",  // ScreenMainMenuAnd1PRaceFlow
            "004275a0",  // RunAttractModeDemoScreen
            "00414b50",  // RunFrontendDisplayLoop
            "00417d50",  // ScreenExtrasGallery (gallery/slideshow)
            "00414610",  // SetFrontendScreen
        };
        
        String[] labels = {
            "ScreenMainMenuAnd1PRaceFlow",
            "RunAttractModeDemoScreen",
            "RunFrontendDisplayLoop",
            "ScreenExtrasGallery",
            "SetFrontendScreen",
        };
        
        println("\n=== DECOMPILED FUNCTIONS ===");
        for (int i = 0; i < targets.length; i++) {
            try {
                Address a = currentProgram.getAddressFactory()
                                          .getDefaultAddressSpace()
                                          .getAddress(targets[i]);
                Function fn = fm.getFunctionAt(a);
                if (fn == null) fn = fm.getFunctionContaining(a);
                if (fn == null) {
                    println("\n--- " + labels[i] + " @ 0x" + targets[i] + ": NO FUNCTION ---");
                    continue;
                }
                DecompileResults res = decompiler.decompileFunction(fn, 60, monitor);
                String src = res.getDecompiledFunction() != null ? 
                             res.getDecompiledFunction().getC() : "[decompile failed]";
                println("\n--- " + labels[i] + " @ " + fn.getEntryPoint() + " ---");
                // Print up to 120 lines
                String[] lines = src.split("\n");
                int limit = Math.min(120, lines.length);
                for (int j = 0; j < limit; j++) {
                    println(lines[j]);
                }
                if (lines.length > 120) {
                    println("  ... (" + (lines.length - 120) + " more lines)");
                }
            } catch (Exception e) {
                println("  ERROR decompiling " + labels[i] + ": " + e.getMessage());
            }
        }
        
        // 3. Search for global variables related to slideshow
        println("\n=== GLOBAL SYMBOLS SEARCH ===");
        SymbolTable st = currentProgram.getSymbolTable();
        SymbolIterator si = st.getAllSymbols(true);
        while (si.hasNext()) {
            Symbol sym = si.next();
            String name = sym.getName().toLowerCase();
            if ((name.contains("slide") || name.contains("gallery") || 
                 name.contains("attract") || name.contains("background") ||
                 name.contains("anim") || name.contains("frontend")) &&
                sym.getSymbolType() != SymbolType.FUNCTION &&
                sym.getSymbolType() != SymbolType.LABEL) {
                println("  " + sym.getAddress() + "  " + sym.getSymbolType() + "  " + sym.getName());
            }
        }
        
        decompiler.dispose();
        println("\n=== DONE ===");
    }
}
