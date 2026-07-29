// CallersOfStep - walk up from FFrame::Step to ProcessInternal and ProcessEvent.
//
// FUN_00487950 (37 bytes) indexes GNatives[opcode] and calls through it = FFrame::Step.
// Its callers are the script VM entry points: ProcessInternal (the bytecode loop) and
// CallFunction. ProcessEvent is one level above those.
//
// Run:  .\tools\ghidra.ps1 -Script CallersOfStep.java
//@category Singularity

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.ReferenceIterator;

import java.io.PrintWriter;
import java.util.LinkedHashSet;
import java.util.Set;

public class CallersOfStep extends GhidraScript {

    private static final String STEP = "00487950";

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter("R:\\SingularityVR-Dev\\ghidra_projects\\callers_of_step.txt");

        Function step = getFunctionAt(addr(STEP));
        emit(out, "=== FFrame::Step candidate ===");
        emit(out, "  " + step.getName() + " entry=" + step.getEntryPoint() +
                  " size=" + step.getBody().getNumAddresses());
        decompile(out, step);

        emit(out, "");
        emit(out, "=== LEVEL 1: direct callers of Step (ProcessInternal / CallFunction) ===");
        Set<Function> level1 = callersOf(step);
        for (Function f : level1) {
            emit(out, String.format("  %-16s entry=%s size=%-7d params=%d",
                    f.getName(), f.getEntryPoint(), f.getBody().getNumAddresses(), f.getParameterCount()));
        }

        emit(out, "");
        emit(out, "=== LEVEL 2: callers of those (ProcessEvent should appear here) ===");
        for (Function f : level1) {
            emit(out, "");
            emit(out, "--- callers of " + f.getName() +
                      " (size " + f.getBody().getNumAddresses() + ") ---");
            Set<Function> lvl2 = callersOf(f);
            if (lvl2.isEmpty()) emit(out, "  (none direct - reached via vtable)");
            int n = 0;
            for (Function c : lvl2) {
                if (n++ > 20) { emit(out, "  ..."); break; }
                emit(out, String.format("  %-16s entry=%s size=%-7d params=%d",
                        c.getName(), c.getEntryPoint(), c.getBody().getNumAddresses(), c.getParameterCount()));
            }
        }

        Function biggest = null;
        for (Function f : level1)
            if (biggest == null || f.getBody().getNumAddresses() > biggest.getBody().getNumAddresses())
                biggest = f;
        if (biggest != null) {
            emit(out, "");
            emit(out, "=== decompiled largest Step caller (" + biggest.getName() + ") ===");
            decompile(out, biggest);
        }

        out.close();
        println("wrote R:\\SingularityVR-Dev\\ghidra_projects\\callers_of_step.txt");
    }

    private Set<Function> callersOf(Function f) {
        Set<Function> s = new LinkedHashSet<>();
        ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(f.getEntryPoint());
        while (it.hasNext()) {
            Function cf = getFunctionContaining(it.next().getFromAddress());
            if (cf != null) s.add(cf);
        }
        return s;
    }

    private void decompile(PrintWriter out, Function f) {
        if (f.getBody().getNumAddresses() > 25000) { emit(out, "  (too large to decompile)"); return; }
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        DecompileResults res = di.decompileFunction(f, 120, monitor);
        if (res != null && res.decompileCompleted()) emit(out, res.getDecompiledFunction().getC());
        else emit(out, "  (decompile failed)");
        di.dispose();
    }

    private void emit(PrintWriter out, String s) { out.println(s); println(s); }

    private Address addr(String hex) {
        return currentProgram.getAddressFactory().getDefaultAddressSpace()
                .getAddress(Long.parseLong(hex, 16));
    }
}
