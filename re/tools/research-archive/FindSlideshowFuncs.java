// Find slideshow-related functions in TD5 by string/data xrefs
// @category TD5RE

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.mem.*;
import java.util.*;
import java.util.stream.*;

public class FindSlideshowFuncs extends GhidraScript {
    private DecompInterface decompiler;

    @Override
    public void run() throws Exception {
        println("=== TD5 Frontend Slideshow - Function Discovery ===");

        decompiler = new DecompInterface();
        DecompileOptions opts = new DecompileOptions();
        decompiler.setOptions(opts);
        decompiler.openProgram(currentProgram);

        // Find functions by address range
        // Main menu: 0x415490
        // Try to find function bodies around key addresses
        long[] keyAddresses = {
            0x415490L, 0x415000L, 0x415400L, 0x415450L,
            0x4275a0L, 0x427000L, 0x427500L, 0x427200L,
            0x417d50L, 0x417000L, 0x417500L,
            0x414b50L, 0x414610L
        };
        
        println("\n=== FUNCTION DISCOVERY ===");
        Set<Long> decompiledFns = new HashSet<>();
        FunctionManager fm = currentProgram.getFunctionManager();
        
        for (long va : keyAddresses) {
            Address a = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(va);
            Function fn = fm.getFunctionContaining(a);
            if (fn != null && !decompiledFns.contains(fn.getEntryPoint().getOffset())) {
                println("  Found function at " + fn.getEntryPoint() + " (query: 0x" + 
                        Long.toHexString(va) + "): size=" + fn.getBody().getNumAddresses());
                decompiledFns.add(fn.getEntryPoint().getOffset());
            }
        }
        
        // List all functions in range 0x414000 - 0x428000 (frontend area)
        println("\n=== FRONTEND FUNCTION RANGE (0x414000 - 0x428000) ===");
        Address start = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(0x414000L);
        Address end   = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(0x428000L);
        
        List<Function> frontendFns = new ArrayList<>();
        for (Function fn : fm.getFunctions(start, true)) {
            if (fn.getEntryPoint().compareTo(end) > 0) break;
            frontendFns.add(fn);
            println("  " + fn.getEntryPoint() + "  size=" + fn.getBody().getNumAddresses() + "  " + fn.getName());
        }
        
        // Decompile key functions
        println("\n=== DECOMPILE KEY FUNCTIONS ===");
        long[] decompTargets = {
            0x414b50L,  // RunFrontendDisplayLoop
            0x414610L,  // SetFrontendScreen
            0x415490L,  // ScreenMainMenuAnd1PRaceFlow
            0x4275a0L,  // RunAttractModeDemoScreen
            0x417d50L,  // ScreenExtrasGallery
        };
        String[] decompLabels = {
            "RunFrontendDisplayLoop (0x414b50)",
            "SetFrontendScreen (0x414610)",
            "ScreenMainMenuAnd1PRaceFlow (0x415490)",
            "RunAttractModeDemoScreen (0x4275a0)",
            "ScreenExtrasGallery (0x417d50)",
        };
        
        for (int i = 0; i < decompTargets.length; i++) {
            Address a = currentProgram.getAddressFactory().getDefaultAddressSpace()
                                      .getAddress(decompTargets[i]);
            Function fn = fm.getFunctionContaining(a);
            if (fn == null) {
                println("\n--- " + decompLabels[i] + ": NOT FOUND ---");
                // Try creating it
                Function created = createFunction(a, null);
                if (created != null) {
                    println("  Created function at " + a);
                    fn = created;
                }
            }
            if (fn == null) continue;
            
            DecompileResults res = decompiler.decompileFunction(fn, 120, monitor);
            if (res.getDecompiledFunction() == null) {
                println("\n--- " + decompLabels[i] + ": DECOMPILE FAILED ---");
                continue;
            }
            String src = res.getDecompiledFunction().getC();
            String[] lines = src.split("\n");
            println("\n--- " + decompLabels[i] + " @ " + fn.getEntryPoint() + 
                    " (size=" + fn.getBody().getNumAddresses() + ") ---");
            int limit = Math.min(200, lines.length);
            for (int j = 0; j < limit; j++) println(lines[j]);
            if (lines.length > 200) println("  ... (" + (lines.length-200) + " more lines)");
        }
        
        // Search for string references to Pic1/Pic2/Extras/gallery
        println("\n=== STRING SEARCH FOR SLIDESHOW CONTENT ===");
        Memory mem = currentProgram.getMemory();
        MemoryBlock[] blocks = mem.getBlocks();
        for (MemoryBlock blk : blocks) {
            if (!blk.isInitialized() || !blk.isRead()) continue;
            // Only search .rdata and .data
            String bname = blk.getName();
            if (!bname.contains("data") && !bname.contains("rdata") && !bname.equals(".text")) continue;
            
            byte[] data;
            try {
                long size = blk.getSize();
                if (size > 0x100000) continue;
                data = new byte[(int)size];
                blk.getBytes(blk.getStart(), data);
            } catch (Exception e) { continue; }
            
            // Search for "Pic", "gallery", "Extras", "slide" as ASCII
            String[] searchTerms = {"Pic", "gallery", "Extras", "Gallery", "slide", "attract",
                                    "Front End/", "FrontEnd", "MainMenu", "background"};
            for (String term : searchTerms) {
                byte[] needle = term.getBytes();
                for (int j = 0; j < data.length - needle.length; j++) {
                    boolean match = true;
                    for (int k = 0; k < needle.length && match; k++) {
                        if (data[j+k] != needle[k]) match = false;
                    }
                    if (match) {
                        // Check it's a proper string (printable chars follow)
                        long va = blk.getStart().getOffset() + j;
                        // Read more chars
                        StringBuilder sb = new StringBuilder();
                        for (int k = j; k < Math.min(j+64, data.length) && data[k] != 0; k++) {
                            if (data[k] >= 0x20 && data[k] < 0x7F) sb.append((char)data[k]);
                            else break;
                        }
                        if (sb.length() >= term.length()) {
                            println("  0x" + Long.toHexString(va) + "  \"" + sb + "\"");
                        }
                        j += term.length(); // skip ahead
                    }
                }
            }
        }
        
        decompiler.dispose();
        println("\n=== DONE ===");
    }
}
