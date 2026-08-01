// FindInterpreterCallers - the fourth and hopefully last hop to UObject::ProcessEvent.
//
// 0x012a72c0 contains BOTH "Error: CallFunction - ..." strings and is 15,209 bytes. That size is
// the finding, not a nuisance: UObject::CallFunction is a few hundred bytes in UE3, so this is not
// CallFunction alone - it is the bytecode interpreter with CallFunction inlined into it. Which is
// exactly what ENGINE_NOTES deduced from the other end: opcodes 0x00-0x35 are all execUndefined in
// GNatives, yet the game runs, so this build must handle them in one big inlined switch. Here it
// is.
//
// That makes 0x012a72c0 effectively UObject::ProcessInternal, and ProcessEvent is one of its four
// callers. Telling them apart:
//
//   * ProcessEvent has MANY callers of its own - native code invokes script from everywhere.
//   * The state-code path and any thin forwarders have few.
//
// So print each caller with its own fan-in, and decompile the plausible ones.
//
// Run:  .\tools\ghidra.ps1 -Script FindInterpreterCallers.java
//@category Singularity

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

import java.io.PrintWriter;
import java.util.Set;

public class FindInterpreterCallers extends GhidraScript {

    private PrintWriter out;

    @Override
    public void run() throws Exception {
        out = new PrintWriter("R:\\SingularityVR-Dev\\ghidra_projects\\interpreter_callers.txt");

        Function interp = getFunctionAt(addr("012a72c0"));
        if (interp == null) { emit("FATAL: no function at 012a72c0"); out.close(); return; }

        emit("=== the interpreter (CallFunction inlined) ===");
        emit("  entry=" + interp.getEntryPoint() + " size=" + interp.getBody().getNumAddresses());

        Set<Function> callers = interp.getCallingFunctions(monitor);
        emit("");
        emit("=== its " + callers.size() + " callers - ProcessEvent is the one with high fan-in ===");
        for (Function c : callers) {
            int fanin = c.getCallingFunctions(monitor).size();
            emit(String.format("  %s  size=%-6d  own callers=%-5d %s",
                    c.getEntryPoint(), c.getBody().getNumAddresses(), fanin,
                    fanin > 50 ? "  <== PROCESSEVENT CANDIDATE" : ""));
        }

        // Decompile every caller small enough to read. ProcessEvent sets up an FFrame, checks
        // FUNC_Native, allocates locals from Function->PropertiesSize and zeroes them - all
        // recognisable in the C output even without symbols.
        emit("");
        emit("=== callers decompiled ===");
        for (Function c : callers) {
            emit("");
            emit("--- " + c.getEntryPoint() + " (size " + c.getBody().getNumAddresses()
                 + ", own callers " + c.getCallingFunctions(monitor).size() + ") ---");
            decompile(c, 4000);
        }

        out.close();
        println("wrote R:\\SingularityVR-Dev\\ghidra_projects\\interpreter_callers.txt");
    }

    private void decompile(Function f, int maxSize) {
        if (f.getBody().getNumAddresses() > maxSize) {
            emit("  (" + f.getBody().getNumAddresses() + " bytes - too big to print)");
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
