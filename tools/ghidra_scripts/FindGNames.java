// FindGNames - locate GNames (and hopefully GObjects) from the FName constructor.
//
// FName::FName at 0x004affd0 interns a string into the global name table, so it must touch
// GNames directly. Decompile it and its callees, and report the .data globals it references -
// the name table base and its hash head will be among them.
//
// Run:  .\tools\ghidra.ps1 -Script FindGNames.java
//@category Singularity

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.scalar.Scalar;

import java.io.PrintWriter;
import java.util.LinkedHashMap;
import java.util.Map;

public class FindGNames extends GhidraScript {

    private static final String FNAME_CTOR = "004affd0";

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter("R:\\SingularityVR-Dev\\ghidra_projects\\gnames.txt");

        Function ctor = getFunctionContaining(addr(FNAME_CTOR));
        if (ctor == null) { emit(out, "no function at " + FNAME_CTOR); out.close(); return; }

        emit(out, "=== FName::FName candidate ===");
        emit(out, "  " + ctor.getName() + " entry=" + ctor.getEntryPoint() +
                  " size=" + ctor.getBody().getNumAddresses());

        // globals referenced by the ctor and everything it calls (one level down)
        Map<Long, String> globals = new LinkedHashMap<>();
        collectGlobals(ctor, globals, "ctor");
        for (Function callee : ctor.getCalledFunctions(monitor)) {
            if (callee.getBody().getNumAddresses() < 4000) {
                collectGlobals(callee, globals, callee.getName());
            }
        }

        emit(out, "");
        emit(out, "=== writable-section globals touched (GNames candidates) ===");
        globals.entrySet().stream()
            .sorted(Map.Entry.comparingByKey())
            .forEach(e -> emit(out, String.format("  %08X   (via %s)", e.getKey(), e.getValue())));

        emit(out, "");
        emit(out, "=== decompiled FName::FName ===");
        decompile(out, ctor);

        emit(out, "");
        emit(out, "=== callees ===");
        for (Function callee : ctor.getCalledFunctions(monitor)) {
            emit(out, "  " + callee.getName() + " entry=" + callee.getEntryPoint() +
                      " size=" + callee.getBody().getNumAddresses());
        }

        out.close();
        println("wrote R:\\SingularityVR-Dev\\ghidra_projects\\gnames.txt");
    }

    private void collectGlobals(Function f, Map<Long, String> into, String tag) {
        Instruction ins = currentProgram.getListing().getInstructionAt(f.getEntryPoint());
        int guard = 0;
        while (ins != null && f.getBody().contains(ins.getAddress()) && guard++ < 4000) {
            for (int op = 0; op < ins.getNumOperands(); op++) {
                for (Object o : ins.getOpObjects(op)) {
                    long v = -1;
                    if (o instanceof Scalar) v = ((Scalar) o).getUnsignedValue();
                    else if (o instanceof Address) v = ((Address) o).getOffset();
                    if (v > 0x1000000L && v < 0x2000000L) {
                        MemoryBlock b = currentProgram.getMemory().getBlock(addr(Long.toHexString(v)));
                        if (b != null && b.isWrite() && !into.containsKey(v)) into.put(v, tag);
                    }
                }
            }
            ins = ins.getNext();
        }
    }

    private void decompile(PrintWriter out, Function f) {
        if (f.getBody().getNumAddresses() > 20000) { emit(out, "  (too large)"); return; }
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        DecompileResults r = di.decompileFunction(f, 120, monitor);
        emit(out, (r != null && r.decompileCompleted()) ? r.getDecompiledFunction().getC()
                                                        : "  (decompile failed)");
        di.dispose();
    }

    private void emit(PrintWriter out, String s) { out.println(s); println(s); }

    private Address addr(String hex) {
        return currentProgram.getAddressFactory().getDefaultAddressSpace()
                .getAddress(Long.parseLong(hex, 16));
    }
}
