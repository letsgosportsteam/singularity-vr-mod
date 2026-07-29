// FindCameraHooks - locate UE3 camera/view functions in Singularity.exe
//
// Strategy: the UE3 class/function names survive in .rdata as UTF-16 strings (confirmed by
// direct binary scan: RvPlayerCamera x23, PlayerCamera x25, CalcCamera x6,
// GetPlayerViewPoint x4, UpdateViewTarget x2, ProcessViewRotation x1, FOVAngle x3).
// UE3 registers native classes and properties by name at startup, so code that references
// one of these strings is registration/reflection code sitting near the real implementation.
// Find the strings, walk their cross-references, report the containing functions.
//
// This does NOT immediately give the hook address - it gives a short, ranked list of places
// to look, which beats scrolling 20 MB of .text.
//
// Run:  .\tools\ghidra.ps1 -Script FindCameraHooks.java
//@category Singularity

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

public class FindCameraHooks extends GhidraScript {

    // Ordered roughly by how promising each is as a hook point.
    private static final String[] TARGETS = {
        "RvPlayerCamera",       // Raven's own camera subclass - most specific
        "PlayerCamera",
        "CalcCamera",
        "GetPlayerViewPoint",
        "UpdateViewTarget",
        "ProcessViewRotation",
        "UpdateRotation",
        "FOVAngle",
        "ViewTarget",
        "RvPlayerController",
        "RvWeapon",
        "RvPawn",
    };

    @Override
    public void run() throws Exception {
        println("=======================================================");
        println(" FindCameraHooks - " + currentProgram.getName());
        println(" image base: " + currentProgram.getImageBase());
        println("=======================================================");

        Memory mem = currentProgram.getMemory();

        for (String name : TARGETS) {
            if (monitor.isCancelled()) return;
            println("");
            println("--- " + name + " ---");

            List<Address> hits = findAllUtf16(mem, name);
            if (hits.isEmpty()) {
                println("  (no UTF-16 occurrence found)");
                continue;
            }
            println("  " + hits.size() + " string occurrence(s)");

            Set<String> reported = new LinkedHashSet<>();
            int totalRefs = 0;

            for (Address strAddr : hits) {
                if (monitor.isCancelled()) return;
                ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(strAddr);
                while (it.hasNext()) {
                    Reference ref = it.next();
                    totalRefs++;
                    Address from = ref.getFromAddress();
                    Function f = getFunctionContaining(from);
                    String line = (f != null)
                        ? String.format("  xref @ %s  in  %s  (entry %s)", from, f.getName(), f.getEntryPoint())
                        : String.format("  xref @ %s  (no containing function - likely a data table)", from);
                    if (reported.add(line)) println(line);
                }
            }

            if (totalRefs == 0) {
                println("  string present but NO code references.");
                println("  -> likely a bare FName table entry; the implementation is reached");
                println("     through the name table at runtime, not by direct pointer.");
            }
        }

        println("");
        println("=======================================================");
        println(" Next: inspect the reported functions in the GUI.");
        println(" A camera function should take/return an FVector position and");
        println(" an FRotator (3 x int32, 65536 units per turn) - that signature");
        println(" shape is the strongest confirmation.");
        println("=======================================================");
    }

    /** Find every occurrence of a string encoded as UTF-16LE in initialised memory. */
    private List<Address> findAllUtf16(Memory mem, String s) throws Exception {
        byte[] pattern = new byte[s.length() * 2];
        for (int i = 0; i < s.length(); i++) {
            pattern[i * 2] = (byte) s.charAt(i);
            pattern[i * 2 + 1] = 0;
        }

        List<Address> out = new ArrayList<>();
        for (MemoryBlock block : mem.getBlocks()) {
            if (!block.isInitialized()) continue;
            Address at = block.getStart();
            Address end = block.getEnd();
            while (at != null && at.compareTo(end) < 0) {
                if (monitor.isCancelled()) return out;
                Address found = mem.findBytes(at, end, pattern, null, true, monitor);
                if (found == null) break;
                out.add(found);
                at = found.add(2);
                if (out.size() > 200) return out;   // plenty; avoid pathological cases
            }
        }
        return out;
    }
}
