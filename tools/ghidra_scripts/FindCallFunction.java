// FindCallFunction - third pass at the UE3 script-call chokepoint, from a NEW anchor.
//
// Pass 1 (FindScriptVM) found GNatives, execUndefined and FFrame::Step, but Step is 37 bytes and
// inlined everywhere, so it has zero callers and the call graph dead-ends.
// Pass 2 (FindProcessEvent) chased a UTF-16 "recursion" string that turned out to be a Raven OOM
// reporter, unrelated.
//
// The anchor this time is real and was sitting in the ASCII strings the whole time:
//
//     "Error: CallFunction - '%s' is not a function"        @ 0x01A3AEE8, referenced from 0x012A871E
//     "Error: CallFunction - attempt to call invalid function" @ 0x01A3AEB0, referenced from 0x012A8843
//
// Both are UObject::CallFunction's own error paths, so the function containing them IS
// UObject::CallFunction - named, unambiguous, and a direct neighbour of what we want.
//
// What we are after: in UE3, BOTH CallFunction (script calling script) and ProcessEvent (native
// calling script) funnel into UObject::ProcessInternal. So ProcessInternal is a CALLEE of
// CallFunction, and ProcessEvent is one of ProcessInternal's OTHER callers. That makes this a
// two-hop graph walk from a known point rather than a search.
//
// Why we want it: PlayerController.Rotation drives the view, the culling frustum AND the weapon's
// fire trace, and run 80 proved there is no second rotation field to redirect. Separating aim from
// culling therefore has to happen in TIME - set Rotation to the hand on the way into the script
// call that reads it, restore the head on the way out.
//
// Run:  .\tools\ghidra.ps1 -Script FindCallFunction.java
//@category Singularity

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

import java.io.PrintWriter;
import java.util.Set;
import java.util.TreeMap;

public class FindCallFunction extends GhidraScript {

    private PrintWriter out;

    @Override
    public void run() throws Exception {
        out = new PrintWriter("R:\\SingularityVR-Dev\\ghidra_projects\\call_function.txt");

        // ---- 1. the anchor ----
        Function cf = getFunctionContaining(addr("012A871E"));
        if (cf == null) {
            emit("FATAL: no function contains 012A871E - the string xref did not land in code");
            out.close();
            return;
        }
        emit("=== UObject::CallFunction (from its own error string) ===");
        describe(cf);

        // Confirm the second string lands in the SAME function. If it does not, the boundary
        // detection is wrong and everything downstream is built on sand.
        Function cf2 = getFunctionContaining(addr("012A8843"));
        emit("  second error string is in: " +
             (cf2 == null ? "<none>" : cf2.getEntryPoint().toString()) +
             (cf2 != null && cf2.getEntryPoint().equals(cf.getEntryPoint())
                 ? "   <-- SAME function, anchor confirmed"
                 : "   <-- DIFFERENT function, treat the anchor with suspicion"));

        // ---- 2. callees, ranked by how few callers they have ----
        //
        // ProcessInternal should have a small, specific caller set - CallFunction, ProcessEvent,
        // and perhaps a state-code path. A callee with hundreds of callers is a utility
        // (appMalloc, memcpy, FName plumbing) and is noise here.
        emit("");
        emit("=== callees of CallFunction, by caller count (ProcessInternal should be 2-6) ===");
        TreeMap<Integer, StringBuilder> byCount = new TreeMap<>();
        Set<Function> callees = cf.getCalledFunctions(monitor);
        for (Function ce : callees) {
            int n = ce.getCallingFunctions(monitor).size();
            byCount.computeIfAbsent(n, k -> new StringBuilder())
                   .append(String.format("    %s  size=%-6d callers=%d%n",
                           ce.getEntryPoint(), ce.getBody().getNumAddresses(), n));
        }
        for (Integer n : byCount.keySet()) {
            if (n > 40) continue;                    // utilities, not what we are after
            emit("  --- " + n + " caller(s) ---");
            out.print(byCount.get(n));
            println(byCount.get(n).toString());
        }

        // ---- 3. for the plausible ones, who ELSE calls them ----
        //
        // This is the payoff: ProcessEvent is the caller of ProcessInternal that is NOT
        // CallFunction. Sizes are printed because ProcessEvent is substantial (frame setup,
        // native check, locals allocation) rather than a thin forwarder.
        emit("");
        emit("=== other callers of each low-fanin callee - ProcessEvent is in here ===");
        for (Function ce : callees) {
            Set<Function> callers = ce.getCallingFunctions(monitor);
            if (callers.size() < 2 || callers.size() > 12) continue;
            emit("");
            emit("  callee " + ce.getEntryPoint() + " (size " + ce.getBody().getNumAddresses()
                 + ", " + callers.size() + " callers):");
            for (Function c : callers) {
                boolean self = c.getEntryPoint().equals(cf.getEntryPoint());
                emit(String.format("      %s  size=%-6d %s", c.getEntryPoint(),
                        c.getBody().getNumAddresses(), self ? "<-- CallFunction itself" : ""));
            }
        }

        // ---- 4. decompile CallFunction if it is small enough to read ----
        emit("");
        emit("=== CallFunction decompiled ===");
        decompile(cf, 6000);

        out.close();
        println("wrote R:\\SingularityVR-Dev\\ghidra_projects\\call_function.txt");
    }

    private void describe(Function f) {
        emit("  entry=" + f.getEntryPoint()
             + "  size=" + f.getBody().getNumAddresses()
             + "  name=" + f.getName()
             + "  params=" + f.getParameterCount()
             + "  callers=" + f.getCallingFunctions(monitor).size());
    }

    private void decompile(Function f, int maxSize) {
        if (f.getBody().getNumAddresses() > maxSize) {
            emit("  (function is " + f.getBody().getNumAddresses() + " bytes - too big to read here)");
            return;
        }
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        DecompileResults res = di.decompileFunction(f, 120, monitor);
        if (res != null && res.decompileCompleted()) emit(res.getDecompiledFunction().getC());
        else emit("  (decompile failed)");
        di.dispose();
    }

    private void emit(String s) { out.println(s); println(s); }

    private Address addr(String hex) {
        return currentProgram.getAddressFactory().getDefaultAddressSpace()
                .getAddress(Long.parseLong(hex, 16));
    }
}
